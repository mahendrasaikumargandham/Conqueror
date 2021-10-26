#include <syscall.h>
#include <fcntl.h>
#include <lib.h>

int main() 
{
	char *msg = "Conqueror_1.0 Initializing";
	sleep_sec(1);
	
	str_print(msg);
	sleep_sec(1);
	execute_process("/bin/lash");
	while(1)
	{
		sleep_sec(1);
	}
	return 0;
}
