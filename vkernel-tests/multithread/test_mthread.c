#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>

#define NUM_THREADS 3
#define NR_ITER 4

pthread_key_t priority_key;

void do_work(void *threadid)
{
	int i;
	long tid;
	char str[10];	
	tid = (long)threadid;
	sigset_t mask;
	int es;

	/* Set thread id as thread specific data */	
	pthread_setspecific(priority_key, malloc(sizeof(int)));
	*((int*)pthread_getspecific(priority_key)) = tid;
	
	if (tid == 1) {
		sigset_t signal_mask;
		sigemptyset(&signal_mask);
		sigaddset(&signal_mask, SIGINT);
		sigaddset(&signal_mask, SIGCKPT);
		sigaddset(&signal_mask, SIGTERM);
		pthread_sigmask (SIG_BLOCK, &signal_mask, NULL);
	}
	
	for(i = 0; i < NR_ITER; i++) {
		sleep(2);
		/* Get thread specific data */
		snprintf(str, 9, "%i", *((int*)pthread_getspecific(priority_key)));
		str[9] = 0;
		/* Get stack pointer */
		asm volatile ("movl %%esp, %0" : "=r"(es));
		/* Get signal mask */
		pthread_sigmask (SIG_BLOCK, NULL, &mask);
		
		printf("[Thread %s] Hello from iteration %i!\n Stack pointer %x, signal mask %8.8lx.\n",
			 str, i, es, mask);
	}
	pthread_exit(NULL);
}

int main()
{
	pthread_t threads[NUM_THREADS];
	int rc;
	long t;

	pthread_key_create(&priority_key, NULL);

	for (t = 1; t <= NUM_THREADS; t++) {
		rc = pthread_create(&threads[t], NULL, do_work, (void *)t);
		if (rc) {
			printf("ERR on create\n");
			exit(-1);
		}
	}

	for (t = 0; t < NUM_THREADS; t++) {
		pthread_join(threads[t], NULL);
	}
	pthread_exit(NULL);
}
