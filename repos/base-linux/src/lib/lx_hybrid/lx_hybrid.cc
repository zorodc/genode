/*
 * \brief  Supplemental code for hybrid Genode/Linux components
 * \author Norman Feske
 * \date   2011-09-02
 */

/*
 * Copyright (C) 2011-2013 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <base/printf.h>
#include <base/component.h>
#include <linux_syscalls.h>
#include <linux_cpu_session/linux_cpu_session.h>

/* base-internal includes */
#include <base/internal/native_thread.h>


extern "C" int raw_write_str(const char *str);

/**
 * Define stack area
 */
Genode::addr_t _stack_area_start;


enum { verbose_atexit = false };


/**
 * Dummy for symbol that is normally provided by '_main.cc'
 */
int genode___cxa_atexit(void (*func)(void*), void *arg, void *dso)
{
	if (verbose_atexit)
		raw_write_str("genode___cxa_atexit called, not implemented\n");
	return 0;
}


/*
 * Manually initialize the 'lx_environ' pointer. For non-hybrid programs, this
 * pointer is initialized by the startup code.
 */
extern char **environ;
extern char **lx_environ;

static void empty_signal_handler(int) { }

/*
 * This function must be called before any other static constructor in the Genode
 * application, so it gets the highest priority (lowest priority number >100)
 */
__attribute__((constructor(101))) void lx_hybrid_init()
{
	lx_environ = environ;

	/*
	 * Set signal handler such that canceled system calls get not
	 * transparently retried after a signal gets received.
	 */
	lx_sigaction(LX_SIGUSR1, empty_signal_handler);
}

namespace Genode {
	extern void bootstrap_component();
	extern void call_global_static_constructors();

	/*
	 * Hook for intercepting the call of the 'Component::construct' method. By
	 * hooking this function pointer in a library constructor, the libc is able
	 * to create a task context for the component code. This context is
	 * scheduled by the libc in a cooperative fashion, i.e. when the
	 * component's entrypoint is activated.
	 */

	extern void (*call_component_construct)(Genode::Environment &) __attribute__((weak));
}

static void lx_hybrid_component_construct(Genode::Environment &env)
{
	Component::construct(env);
}

void (*Genode::call_component_construct)(Genode::Environment &) = &lx_hybrid_component_construct;

/*
 * Static constructors are handled by the Linux startup code - so implement
 * this as empty function.
 */
void Genode::call_global_static_constructors() { }

/*
 * Hybrid components are not allowed to implement legacy main(). This enables
 * us to hook in and bootstrap components as usual.
 */

int main()
{
	Genode::bootstrap_component();

	/* never reached */
}


/************
 ** Thread **
 ************/

/*
 * For hybrid Linux/Genode programs, Genode's thread API is implemented via
 * POSIX threads.
 *
 * Hybrid Linux/Genode programs are linked against the glibc along with other
 * native Linux libraries. Such libraries may use the 'pthread' API to spawn
 * threads, which then may call Genode code. Vice versa, Genode threads may
 * interact with code of a native Linux libraries. Hence, both worlds Genode
 * and native Linux libraries should use the same underlying threading API.
 * Furthermore, using the pthread API is a precondition to satisfy the glibc's
 * assumption about thread-local storage, which is particularly important
 * for the correct thread-local handling of 'errno'. As another benefit of
 * using the pthread API over the normal Genode thread implementation, hybrid
 * Linux/Genode programs comply with the GNU debugger's expectations. Such
 * programs can be debugged as normal Linux programs.
 *
 * Genode's normal thread API for Linux was introduced to decouple Genode
 * from the glibc. This is especially important when using Genode's libc
 * Mixing both Genode's libc and glibc won't work.
 */

/* Genode includes */
#include <base/thread.h>
#include <base/env.h>

/* libc includes */
#include <pthread.h>
#include <stdio.h>
#include <errno.h>

using namespace Genode;


/**
 * Return TLS key used to storing the thread meta data
 */
static pthread_key_t tls_key()
{
	struct Tls_key
	{
		pthread_key_t key;

		Tls_key()
		{
			pthread_key_create(&key, 0);
		}
	};

	static Tls_key inst;
	return inst.key;
}


namespace Genode {

	/**
	 * Meta data tied to the thread via the pthread TLS mechanism
	 */
	struct Native_thread::Meta_data
	{
		/**
		 * Linux-specific thread meta data
		 *
		 * For non-hybrid programs, this information is located at the
		 * 'Stack'. But the POSIX threads of hybrid programs have no 'Stack'
		 * object. So we have to keep the meta data here.
		 */
		Native_thread native_thread;

		/**
		 * Filled out by 'thread_start' function in the stack of the new
		 * thread
		 */
		Thread_base * const thread_base;

		/**
		 * POSIX thread handle
		 */
		pthread_t pt;

		/**
		 * Constructor
		 *
		 * \param thread  associated 'Thread_base' object
		 */
		Meta_data(Thread_base *thread) : thread_base(thread)
		{
			native_thread.meta_data = this;
		}

		/**
		 * Used to block the constructor until the new thread has initialized
		 * 'id'
		 */
		virtual void wait_for_construction() = 0;
		virtual void constructed() = 0;

		/**
		 * Used to block the new thread until 'start' is called
		 */
		virtual void wait_for_start() = 0;
		virtual void started() = 0;

		/**
		 * Used to block the 'join()' function until the 'entry()' is done
		 */
		virtual void wait_for_join() = 0;
		virtual void joined() = 0;
	};

	/**
	 * Thread meta data for a thread created by Genode
	 */
	class Thread_meta_data_created : public Native_thread::Meta_data
	{
		private:

			/**
			 * Lock with the initial state set to LOCKED
			 */
			struct Barrier : Lock { Barrier() : Lock(Lock::LOCKED) { } };

			/**
			 * Used to block the constructor until the new thread has initialized
			 * 'id'
			 */
			Barrier _construct_lock;

			/**
			 * Used to block the new thread until 'start' is called
			 */
			Barrier _start_lock;

			/**
			 * Used to block the 'join()' function until the 'entry()' is done
			 */
			Barrier _join_lock;

		public:

			Thread_meta_data_created(Thread_base *thread)
			: Native_thread::Meta_data(thread) { }

			void wait_for_construction()
			{
				_construct_lock.lock();
			}

			void constructed()
			{
				_construct_lock.unlock();
			}

			void wait_for_start()
			{
				_start_lock.lock();
			}

			void started()
			{
				_start_lock.unlock();
			}

			void wait_for_join()
			{
				_join_lock.lock();
			}

			void joined()
			{
				_join_lock.unlock();
			}
	};

	/**
	 * Thread meta data for an adopted thread
	 */
	class Thread_meta_data_adopted : public Native_thread::Meta_data
	{
		public:

			Thread_meta_data_adopted(Thread_base *thread)
			: Native_thread::Meta_data(thread) { }

			void wait_for_construction()
			{
				PERR("wait_for_construction() called for an adopted thread");
			}

			void constructed()
			{
				PERR("constructed() called for an adopted thread");
			}

			void wait_for_start()
			{
				PERR("wait_for_start() called for an adopted thread");
			}

			void started()
			{
				PERR("started() called for an adopted thread");
			}

			void wait_for_join()
			{
				PERR("wait_for_join() called for an adopted thread");
			}

			void joined()
			{
				PERR("joined() called for an adopted thread");
			}
	};
}


/**
 * Return Linux-specific extension of the Env::CPU session interface
 */
Linux_cpu_session *cpu_session(Cpu_session * cpu_session)
{
	Linux_cpu_session *cpu = dynamic_cast<Linux_cpu_session *>(cpu_session);

	if (!cpu) {
		PERR("could not obtain Linux extension to CPU session interface");
		struct Could_not_access_linux_cpu_session { };
		throw Could_not_access_linux_cpu_session();
	}

	return cpu;
}


static void adopt_thread(Native_thread::Meta_data *meta_data)
{
	/*
	 * Set signal handler such that canceled system calls get not
	 * transparently retried after a signal gets received.
	 */
	lx_sigaction(LX_SIGUSR1, empty_signal_handler);

	/*
	 * Prevent children from becoming zombies. (SIG_IGN = 1)
	 */
	lx_sigaction(LX_SIGCHLD, (void (*)(int))1);

	/* assign 'Native_thread::Meta_data' pointer to TLS entry */
	pthread_setspecific(tls_key(), meta_data);

	/* enable immediate cancellation when calling 'pthread_cancel' */
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);

	/*
	 * Initialize thread meta data
	 */
	Native_thread &native_thread = meta_data->thread_base->native_thread();
	native_thread.tid = lx_gettid();
	native_thread.pid = lx_getpid();
}


static void *thread_start(void *arg)
{
	Native_thread::Meta_data *meta_data = (Native_thread::Meta_data *)arg;

	adopt_thread(meta_data);

	/* unblock 'Thread_base' constructor */
	meta_data->constructed();

	/* block until the 'Thread_base::start' gets called */
	meta_data->wait_for_start();

	Thread_base::myself()->entry();

	meta_data->joined();
	return 0;
}


extern "C" void *malloc(::size_t size);


Thread_base *Thread_base::myself()
{
	void * const tls = pthread_getspecific(tls_key());

	if (tls != 0)
		return ((Native_thread::Meta_data *)tls)->thread_base;

	bool const is_main_thread = (lx_getpid() == lx_gettid());
	if (is_main_thread)
		return 0;

	/*
	 * The function was called from a thread created by other means than
	 * Genode's thread API. This may happen if a native Linux library creates
	 * threads via the pthread library. If such a thread calls Genode code,
	 * which then tries to perform IPC, the program fails because there exists
	 * no 'Thread_base' object. We recover from this unfortunate situation by
	 * creating a dummy 'Thread_base' object and associate it with the calling
	 * thread.
	 */

	/*
	 * Create dummy 'Thread_base' object but suppress the execution of its
	 * constructor. If we called the constructor, we would create a new Genode
	 * thread, which is not what we want. For the allocation, we use glibc
	 * malloc because 'Genode::env()->heap()->alloc()' uses IPC.
	 *
	 * XXX  Both the 'Thread_base' and 'Native_thread::Meta_data' objects are
	 *      never freed.
	 */
	Thread_base *thread = (Thread_base *)malloc(sizeof(Thread_base));
	memset(thread, 0, sizeof(*thread));
	Native_thread::Meta_data *meta_data = new Thread_meta_data_adopted(thread);

	/*
	 * Initialize 'Thread_base::_native_thread' to point to the default-
	 * constructed 'Native_thread' (part of 'Meta_data').
	 */
	meta_data->thread_base->_native_thread = &meta_data->native_thread;
	adopt_thread(meta_data);

	return thread;
}


void Thread_base::start()
{
	/*
	 * Unblock thread that is supposed to slumber in 'thread_start'.
	 */
	native_thread().meta_data->started();
}


void Thread_base::join()
{
	native_thread().meta_data->wait_for_join();
}


Native_thread &Thread_base::native_thread() { return *_native_thread; }


Thread_base::Thread_base(size_t weight, const char *name, size_t stack_size,
                         Type type, Cpu_session * cpu_sess)
: _cpu_session(cpu_sess)
{
	Native_thread::Meta_data *meta_data =
		new (env()->heap()) Thread_meta_data_created(this);

	_native_thread = &meta_data->native_thread;

	int const ret = pthread_create(&meta_data->pt, 0, thread_start, meta_data);
	if (ret) {
		PERR("pthread_create failed (returned %d, errno=%d)",
		     ret, errno);
		destroy(env()->heap(), meta_data);
		throw Out_of_stack_space();
	}

	native_thread().meta_data->wait_for_construction();

	Linux_cpu_session *cpu = cpu_session(_cpu_session);

	_thread_cap = cpu->create_thread(weight, name);
	cpu->thread_id(_thread_cap, native_thread().pid, native_thread().tid);
}


Thread_base::Thread_base(size_t weight, const char *name, size_t stack_size,
                         Type type)
: Thread_base(weight, name, stack_size, type, env()->cpu_session()) { }

void Thread_base::cancel_blocking()
{
	/*
	 * XXX implement interaction with CPU session
	 */
}


Thread_base::~Thread_base()
{
	bool const needs_join = (pthread_cancel(native_thread().meta_data->pt) == 0);

	if (needs_join) {
		int const ret = pthread_join(native_thread().meta_data->pt, 0);
		if (ret)
			PWRN("pthread_join unexpectedly returned with %d (errno=%d)",
			     ret, errno);
	}

	Thread_meta_data_created *meta_data =
		dynamic_cast<Thread_meta_data_created *>(native_thread().meta_data);

	if (meta_data)
		destroy(env()->heap(), meta_data);

	_native_thread = nullptr;

	/* inform core about the killed thread */
	cpu_session(_cpu_session)->kill_thread(_thread_cap);
}
