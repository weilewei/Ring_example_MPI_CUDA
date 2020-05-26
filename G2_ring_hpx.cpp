#include <vector>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <string>

#include "util_cuda.hpp"
#include "util_mpi.hpp"

#include <hpx/hpx_main.hpp>
#include <hpx/async.hpp>
#include <hpx/execution.hpp>
#include <hpx/lcos/future.hpp>
#include <hpx/local_async.hpp>
#include <hpx/mpi.hpp>
#include <hpx/program_options.hpp>

#define MOD(x,n) ((x) % (n))

int main(int argc, char **argv) {
  int is_initialized_ = -1;
  MPI_Initialized(&is_initialized_);
  if (!is_initialized_)
  {
    int provided = 0;
    constexpr int required = MPI_THREAD_FUNNELED;
    MPI_Init_thread(&argc, &argv, required, &provided);
    if (provided < required)
        throw(std::logic_error("MPI does not provide adequate thread support."));
   }

    int rank, mpi_size;
    MPI_CHECK(MPI_Comm_size(MPI_COMM_WORLD, &mpi_size));
    MPI_CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank));

    hpx::mpi::experimental::enable_user_polling enable_polling;
    hpx::mpi::experimental::executor exec(MPI_COMM_WORLD);

    // sync all processors at the beginning
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

    MPI_Request recv_request;
    MPI_Request send_request;
    MPI_Status status;

    int left_neighbor = MOD((rank-1 + mpi_size), mpi_size);
    int right_neighbor = MOD((rank+1 + mpi_size), mpi_size);

    // number of G2s
    int niter = 1000;

    size_t n_elems = 26214400; // 2 ^ 23
    float* G2 = nullptr;
    float* G4 = nullptr;
    float* sendbuff_G2 = nullptr;
    float* recvbuff_G2 = nullptr;

    G2 = allocate_on_device<float>(n_elems);
    G4 = allocate_on_device<float>(n_elems);
    sendbuff_G2 = allocate_on_device<float>(n_elems);
    recvbuff_G2 = allocate_on_device<float>(n_elems);

    double start_time, end_time;
    // sync all processors at the end
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
    if (rank == 0)
    {
        start_time = MPI_Wtime();
    }
    for(int i = 0; i < niter; i++)
    {
        // generate G2 and fill some value in
        generateG2(G2, rank, n_elems);
        update_local_G4(G2, G4, rank, n_elems);

        // get ready for send
        CudaMemoryCopy(sendbuff_G2, G2, n_elems);
        int send_tag = 1 + rank;
        send_tag = 1 + MOD(send_tag-1, MPI_TAG_UB); // just to be safe, MPI_TAG_UB is largest tag value
        for(int icount=0; icount < (mpi_size-1); icount++)
        {
            // encode the originator rank in the message tag as tag = 1 + originator_irank
            int originator_irank = MOD(((rank-1)-icount + 2*mpi_size), mpi_size);
            int recv_tag = 1 + originator_irank;
            recv_tag = 1 + MOD(recv_tag-1, MPI_TAG_UB); // just to be safe, then 1 <= tag <= MPI_TAG_UB

            auto f_send = hpx::async(exec, MPI_Irecv, recvbuff_G2, n_elems, MPI_FLOAT, left_neighbor, 10);
            auto f_recv = hpx::async(exec, MPI_Isend, sendbuff_G2, n_elems, MPI_FLOAT, right_neighbor, 10);

            auto f1 = hpx::dataflow(hpx::launch::async, [&](auto&& f_recv) {	
	      f_recv.get();
              CudaMemoryCopy(G2, recvbuff_G2, n_elems);
              update_local_G4(G2, G4, rank, n_elems);
	    }, f_recv);
            

	   auto f2 = hpx::dataflow(hpx::launch::async, [&](auto&& f1, auto&& f_send) { 
	    f1.get();
            f_send.get();
            // get ready for send
            CudaMemoryCopy(sendbuff_G2, G2, n_elems);
            send_tag = recv_tag;
	   }, f1, f_send);

	   f2.get();
        }
    }

    if (rank == 0)
    {
        end_time = MPI_Wtime();
        printf("Total time spent on %d iteration: %lf \n", niter, (end_time - start_time));
        printf("Number of ranks: %d, number of float elements %d \n", mpi_size, n_elems);
        printf("Average time spent on 1 iteration: %lf \n", (end_time - start_time) / niter);
    }

    // sync all processors at the end
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

    MPI_CHECK(MPI_Finalize());
}
