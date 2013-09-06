#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>

#include "../../cfg.h"

void runapp(char *const argv[])
{
    int status ;
    pid_t pid ;
    pid=fork();
    if( pid==0 ) {
        // child process
        execv( argv[0], argv );
        exit(1);
    }
    else if( pid>0 ) {
        wait(&status);
    }
}

int main()
{
    char * mcu_firmwarefilename ;
    pid_t  mcu_upd_pid ;
    int res = 0 ;
    char mcufirmwarefile[128] ;
    char mcumsgfile[128] ;

    mcu_firmwarefilename = getenv( "POST_FILE_mcu_firmware_file" );
    if( mcu_firmwarefilename ) {
        FILE * mcufirmfile = fopen( mcu_firmwarefilename, "r" ) ;
        if( mcufirmfile ) {
            if( fgetc(mcufirmfile)==':' ) {
                res=1 ;
            }
            fclose( mcufirmfile );
        }
    }

    FILE * fmsg ;
    // progress file
    sprintf( mcumsgfile, "%s/mcuprog", getenv("DOCUMENT_ROOT") );
    setenv( "MCUPROG", mcumsgfile, 1 );
    fmsg=fopen(mcumsgfile, "w");
    if( fmsg ) {
        fprintf(fmsg,"0");
        fclose( fmsg );
    }

    // msg file
    sprintf( mcumsgfile, "%s/mcumsg", getenv("DOCUMENT_ROOT") );
    setenv( "MCUMSG", mcumsgfile, 1 );
    fmsg=fopen(mcumsgfile, "w");
    if( fmsg ) {
        fprintf(fmsg,"\n");
        fclose( fmsg );
    }

    if( res==0 ) {
        printf( "Invalid firmware file!" );
        return 0;
    }

    // make a hard link of firmware file, so it wont be deleted by http process
    sprintf( mcufirmwarefile, "%s/mcufw", getenv("DOCUMENT_ROOT") );
    link( mcu_firmwarefilename, mcufirmwarefile );

    // install MCU firmware
    mcu_upd_pid = fork() ;
    if( mcu_upd_pid == 0 ) {
        // disable stdin , stdout
        int fd = open("/dev/null", O_RDWR );
        dup2(fd, 0);                 // set dummy stdin stdout, also close old stdin (socket)
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
        // if dvrsvr running, suspend it
        FILE * dvrpidfile ;
        pid_t dvrpid ;
        dvrpidfile=fopen(VAR_DIR"/dvrsvr.pid", "r");
        if( dvrpidfile ) {
            dvrpid=0 ;
            fscanf(dvrpidfile, "%d", &dvrpid);
            fclose( dvrpidfile );
            if( dvrpid>0 ) {
                kill( dvrpid, SIGUSR1 );
            }
        }

        // updating MCU firmware
#ifdef  MCU_ZEUS
        fd = open(mcumsgfile, O_RDWR );
        char * msg ;

        msg = (char *)"Start updating MCU firmware, please wait....\n" ;
        write(fd, msg, strlen(msg)) ;
        msg = (char *)"(Try press RESET button when message \"Synchronizing\" appear)\n\n" ;
        write(fd, msg, strlen(msg)) ;

        char * zargs[10] ;
        zargs[0] = APP_DIR"/ioprocess" ;
        zargs[1] = "-fwreset" ;
        zargs[2] = NULL ;
        runapp(zargs);

        dup2(fd, 1);
        dup2(fd, 2);

        zargs[0] = APP_DIR"/lpc21isp" ;
        zargs[1] = "-try1000" ;				// wait for sync, 1000 times
        zargs[2] = "-wipe" ;				// erase flash
        zargs[3] = "-hex" ;					// input .hex file
        zargs[4] = mcufirmwarefile ;		// firmware file
        zargs[5] = "/dev/ttyS1" ;			// MCU connection port
        zargs[6] = "115200" ;				// MCU connection baud
        zargs[7] = "12000" ;				// MCU clock
        zargs[8] = NULL ;
        runapp(zargs);

        lseek(fd, 0, SEEK_END);
        msg = "\nMCU firmware update finished!\n" ;
        write(fd, msg, strlen(msg)) ;
        msg = "Please reset unit.\n" ;
        write(fd, msg, strlen(msg)) ;
        close( fd );

#else
        execlp( APP_DIR"/ioprocess", "ioprocess", "-fw", mcufirmwarefile, NULL );
#endif
        exit(2);    // error exec
    }

    /*
    if( res ) {
    printf( "MCU firmware update succeed. <br /> Please un-plug FORCEON cable. <br />System reboot... " );
    }
    else {
        printf( "MCU firmware update failed. Reset system!" );
    }

    if( fork() == 0 ) {
        // disable stdin , stdout
        int fd = open("/dev/null", O_RDWR );
        dup2(fd, 0);                 // set dummy stdin stdout, also close old stdin (socket)
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
        // reboot system
        execlp( APP_DIR"/ioprocess", "ioprocess", "-reboot", "5", NULL );
        return 2;
    }
*/
    return 0;
}