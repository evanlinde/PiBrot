#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <mpi.h>
#include "fractal.h"
#include "communication.h"

#define WORKTAG 1
#define DIETAG 2

void get_work(const FRAC_INFO  *info, int *rowsTaken, WORK_DATA *work)
{
    if(*rowsTaken >= info->num_rows){
        work->num_rows = 0;
        return;
    }
    int rows = 8;

    work->start_row = *rowsTaken;
    int num_rows = (*rowsTaken)+rows<info->num_rows?rows:info->num_rows-(*rowsTaken);
    work->num_rows = num_rows;

    *rowsTaken += num_rows;
}

int get_max_work_size(const FRAC_INFO *info)
{
    return 8*info->num_cols;
}

void master_pack_and_send(WORK_DATA *work, char *pack_buffer, int buffer_size)
{
    int position;
        
    //pack and send work       
    position = 0;
    MPI_Pack(&work->start_row,1,MPI_INT,pack_buffer, buffer_size, &position,MPI_COMM_WORLD);
    MPI_Pack(&work->num_rows,1,MPI_INT,pack_buffer, buffer_size, &position,MPI_COMM_WORLD);
    MPI_Send(pack_buffer, position, MPI_PACKED, work->rank, WORKTAG, MPI_COMM_WORLD);
}

int master_recv_and_unpack(WORK_DATA *work, FRAC_INFO *frac_info, char *pack_buffer, int buffer_size)
{
    int tag, position, msg_size;
    MPI_Status status;

    // Recieve and unpack work
    MPI_Recv(pack_buffer, buffer_size, MPI_PACKED, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
   
    // Check tag for work/die
    tag = status.MPI_TAG;
    if(tag == DIETAG) {
        return tag;
    }

    position = 0;
    work->rank = status.MPI_SOURCE;

    MPI_Get_count(&status, MPI_PACKED, &msg_size);
    MPI_Unpack(pack_buffer, msg_size, &position, &work->start_row,1,MPI_INT, MPI_COMM_WORLD);
    MPI_Unpack(pack_buffer, msg_size, &position, &work->num_rows,1,MPI_INT, MPI_COMM_WORLD);
    MPI_Unpack(pack_buffer, msg_size, &position, work->pixels, work->num_rows*frac_info->num_cols, MPI_UNSIGNED_CHAR, MPI_COMM_WORLD);

    return tag;
}

void slave_pack_and_send(WORK_DATA *work, FRAC_INFO *frac_info, char *pack_buffer, int buffer_size)
{
    int position;
        
    //pack and send work       
    position = 0;
    MPI_Pack(&work->start_row,1,MPI_INT,pack_buffer, buffer_size,&position, MPI_COMM_WORLD);
    MPI_Pack(&work->num_rows,1,MPI_INT,pack_buffer, buffer_size,&position, MPI_COMM_WORLD);
    MPI_Pack(work->pixels, work->num_rows*frac_info->num_cols, MPI_UNSIGNED_CHAR, pack_buffer, buffer_size, &position, MPI_COMM_WORLD);
    MPI_Send(pack_buffer, position, MPI_PACKED, 0, WORKTAG, MPI_COMM_WORLD);
}

int slave_recv_and_unpack(WORK_DATA *work, char *pack_buffer, int buffer_size)
{
    int tag, position, msg_size;
    MPI_Status status;

    // Recieve and unpack work
    MPI_Recv(pack_buffer, buffer_size, MPI_PACKED, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
   
    // Check tag for work/die
    tag = status.MPI_TAG;
    if(tag == DIETAG) {
        return tag;
    }

    position = 0;
    MPI_Get_count(&status, MPI_PACKED, &msg_size);
    MPI_Unpack(pack_buffer, msg_size, &position, &work->start_row,1,MPI_INT,MPI_COMM_WORLD);
    MPI_Unpack(pack_buffer, msg_size, &position, &work->num_rows,1,MPI_INT,MPI_COMM_WORLD);

    return tag;
}

void master(const FRAC_INFO *frac_info, const STATE_T *ogl_state)
{
    int ntasks, dest;
    WORK_DATA work_send;
    WORK_DATA work_recv;
    int rowsTaken = 0;

    MPI_Comm_size(MPI_COMM_WORLD, &ntasks);    

    // Allocate buffer to hold received pixel data
    size_t size = sizeof(unsigned char) * (unsigned long)frac_info->num_cols * (unsigned long)get_max_work_size(frac_info);
    work_recv.pixels = (unsigned char*)malloc(size);
    if(!work_recv.pixels) {
        printf("row pixel buffer allocation failed, %lu bytes\n", size);
        exit(1);
    }

    // Allocate pack buffer
    int member_size, empty_size, full_size;
    int position;
    char *buffer;
    MPI_Pack_size(1, MPI_INT, MPI_COMM_WORLD, &member_size);
    empty_size = member_size;
    MPI_Pack_size(1, MPI_INT, MPI_COMM_WORLD, &member_size);
    empty_size += member_size;
    MPI_Pack_size(get_max_work_size(frac_info), MPI_UNSIGNED_CHAR, MPI_COMM_WORLD, &member_size);
    full_size = empty_size + member_size;

    buffer = malloc(full_size);    
    if(!buffer) {
        printf("buffer allocation failed, %d bytes\n",full_size);
        exit(1);
    }

    // Send initial data
    for (dest = 1; dest < ntasks; dest++) {
        //Get next work item
        get_work(frac_info,&rowsTaken,&work_send);

        // Set destination to send work to
        work_send.rank = dest;

        // Pack work and send
        master_pack_and_send(&work_send, buffer, empty_size);        
    }

    printf("sent initial work\n");
    //Get next work item
    get_work(frac_info, &rowsTaken, &work_send);

    while(work_send.num_rows) {

	// Receive work load and unpack
        master_recv_and_unpack(&work_recv, frac_info, buffer, full_size);

        // Update texture with recieved buffer
        update_fractal_rows(ogl_state, 0, work_recv.start_row, work_recv.num_rows, work_recv.pixels);

        // Send more work to the rank we just received from
        work_send.rank = work_recv.rank;

        //pack and send work       
        master_pack_and_send(&work_send, buffer, empty_size);        

        //Get next work item
        get_work(frac_info, &rowsTaken, &work_send);

    }

    // Recieve all remaining work
    for (dest = 1; dest < ntasks; dest++) {

	// Receive work load and unpack
        master_recv_and_unpack(&work_recv, frac_info, buffer, full_size);

        // Update texture with received buffer
	update_fractal_rows(ogl_state, 0,  work_recv.start_row, work_recv.num_rows, work_recv.pixels);

        // Kill slaves
        MPI_Send(0,0,MPI_INT,dest,DIETAG,MPI_COMM_WORLD);
    }

    free(buffer);
    free(work_recv.pixels);
}

void slave(const FRAC_INFO *frac_info)
{
    MPI_Status status;
    int tag, rank;
    WORK_DATA work;
    work.pixels = (unsigned char*)malloc(get_max_work_size(frac_info)*sizeof(unsigned char));  

    // Allocate buffers
    int member_size, empty_size, full_size;
    int position;
    char *buffer;
    MPI_Pack_size(1, MPI_INT, MPI_COMM_WORLD, &member_size);
    empty_size = member_size;
    MPI_Pack_size(1, MPI_INT, MPI_COMM_WORLD, &member_size);
    empty_size += member_size;
    MPI_Pack_size(get_max_work_size(frac_info), MPI_UNSIGNED_CHAR, MPI_COMM_WORLD, &member_size);
    full_size = empty_size+member_size;
    buffer = malloc(full_size);

    while(1) {
	// Receive work load and unpack
        tag = slave_recv_and_unpack(&work, buffer, empty_size);

        // Check tag for work/die
        if(tag == DIETAG) {
            free(buffer);
            return;
        }

        // calcPixels
        calcPixels(frac_info, &work);        

        // Pack and send data back
        slave_pack_and_send(&work, frac_info, buffer, full_size);
    }
}
