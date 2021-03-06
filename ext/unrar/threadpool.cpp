#include "rar.hpp"

#ifdef RAR_SMP
#include "threadmisc.cpp"

#ifdef _WIN_ALL
int ThreadPool::ThreadPriority=THREAD_PRIORITY_NORMAL;
#endif

ThreadPool::ThreadPool(uint MaxThreads)
{
  MaxAllowedThreads = MaxThreads;
  if (MaxAllowedThreads>MaxPoolThreads)
    MaxAllowedThreads=MaxPoolThreads;
  if (MaxAllowedThreads==0)
    MaxAllowedThreads=1;

  // If we have more threads than queue size, we'll hang on pool destroying,
  // not releasing all waiting threads.
  if (MaxAllowedThreads>ASIZE(TaskQueue))
    MaxAllowedThreads=ASIZE(TaskQueue);

  Closing=false;

  bool Success;
#ifdef _WIN_ALL
  QueuedTasksCnt=CreateSemaphore(NULL,0,ASIZE(TaskQueue),NULL);
  NoneActive=CreateEvent(NULL,TRUE,TRUE,NULL);
  InitializeCriticalSection(&CritSection); 
  Success=QueuedTasksCnt!=NULL && NoneActive!=NULL;
#elif defined(_UNIX)
  AnyActive = false;
  QueuedTasksCnt = 0;
  Success=pthread_mutex_init(&CritSection,NULL)==0 &&
          pthread_cond_init(&AnyActiveCond,NULL)==0 &&
          pthread_mutex_init(&AnyActiveMutex,NULL)==0 &&
          pthread_cond_init(&QueuedTasksCntCond,NULL)==0 &&
          pthread_mutex_init(&QueuedTasksCntMutex,NULL)==0;
#endif
  if (!Success)
  {
    ErrHandler.GeneralErrMsg(L"\nThread pool initialization failed.");
    ErrHandler.Exit(RARX_FATAL);
  }

  for(uint I=0;I<MaxAllowedThreads;I++)
  {
    ThreadHandles[I] = ThreadCreate( PoolThread, this);
#ifdef _WIN_ALL
    if (ThreadPool::ThreadPriority!=THREAD_PRIORITY_NORMAL)
      SetThreadPriority(ThreadHandles[I],ThreadPool::ThreadPriority);
#endif
  }

  QueueTop = 0;
  QueueBottom = 0;
  ActiveThreads = 0;
}


ThreadPool::~ThreadPool()
{
  WaitDone();
  Closing=true;

#ifdef _WIN_ALL
  ReleaseSemaphore(QueuedTasksCnt,ASIZE(TaskQueue),NULL);
#elif defined(_UNIX)
  // Threads still can access QueuedTasksCnt for a short time after WaitDone(),
  // so lock is required. We would occassionally hang without it.
  pthread_mutex_lock(&QueuedTasksCntMutex);
  QueuedTasksCnt+=ASIZE(TaskQueue);
  pthread_mutex_unlock(&QueuedTasksCntMutex);

  pthread_cond_broadcast(&QueuedTasksCntCond);
#endif

  for(uint I=0;I<MaxAllowedThreads;I++)
  {
#ifdef _WIN_ALL
    // Waiting until the thread terminates.
    CWaitForSingleObject(ThreadHandles[I]);
#endif
    // Close the thread handle. In Unix it is results in pthread_join call,
    // which also waits for thread termination.
    ThreadClose(ThreadHandles[I]);
  }

#ifdef _WIN_ALL
  DeleteCriticalSection(&CritSection);
  CloseHandle(QueuedTasksCnt);
  CloseHandle(NoneActive);
#elif defined(_UNIX)
  pthread_mutex_destroy(&CritSection);
  pthread_cond_destroy(&AnyActiveCond);
  pthread_mutex_destroy(&AnyActiveMutex);
  pthread_cond_destroy(&QueuedTasksCntCond);
  pthread_mutex_destroy(&QueuedTasksCntMutex);
#endif
}


THREAD_TYPE THREAD_ATTR ThreadPool::PoolThread(void *Param)
{
  ((ThreadPool*)Param)->PoolThreadLoop();
}


void ThreadPool::PoolThreadLoop()
{
  QueueEntry Task;
  while (GetQueuedTask(&Task))
  {
    Task.Proc(Task.Param);
    
    CriticalSectionStart(&CritSection); 
    if (--ActiveThreads == 0)
    {
#ifdef _WIN_ALL
      SetEvent(NoneActive);
#elif defined(_UNIX)
      pthread_mutex_lock(&AnyActiveMutex);
      AnyActive=false;
      pthread_cond_signal(&AnyActiveCond);
      pthread_mutex_unlock(&AnyActiveMutex);
#endif
    }
    CriticalSectionEnd(&CritSection); 
  }
}


bool ThreadPool::GetQueuedTask(QueueEntry *Task)
{
#ifdef _WIN_ALL
  CWaitForSingleObject(QueuedTasksCnt);
#elif defined(_UNIX)
  pthread_mutex_lock(&QueuedTasksCntMutex);
  while (QueuedTasksCnt==0)
    cpthread_cond_wait(&QueuedTasksCntCond,&QueuedTasksCntMutex);
  QueuedTasksCnt--;
  pthread_mutex_unlock(&QueuedTasksCntMutex);
#endif

  if (Closing)
    return false;

  CriticalSectionStart(&CritSection); 

  *Task = TaskQueue[QueueBottom];
  QueueBottom = (QueueBottom + 1) % ASIZE(TaskQueue);

  CriticalSectionEnd(&CritSection); 

  return true;
}


// Add task to queue. We assume that it is always called from main thread,
// it allows to avoid any locks here. We process collected tasks only
// when StartWait is called.
void ThreadPool::AddTask(PTHREAD_PROC Proc,void *Data)
{
  // If queue is full, wait until it is empty.
  if ((QueueTop + 1) % ASIZE(TaskQueue) == QueueBottom)
    WaitDone();

  TaskQueue[QueueTop].Proc = Proc;
  TaskQueue[QueueTop].Param = Data;
  QueueTop = (QueueTop + 1) % ASIZE(TaskQueue);
}


// Start queued tasks and wait until all threads are inactive.
// We assume that it is always called from main thread, when pool threads
// are sleeping yet.
void ThreadPool::WaitDone()
{
  // We add ASIZE(TaskQueue) for case if TaskQueue array size is not
  // a power of two. Negative numbers would not suit our purpose here.
  ActiveThreads=(QueueTop+ASIZE(TaskQueue)-QueueBottom) % ASIZE(TaskQueue);
  if (ActiveThreads==0)
    return;
#ifdef _WIN_ALL
  ResetEvent(NoneActive);
  ReleaseSemaphore(QueuedTasksCnt,ActiveThreads,NULL);
  CWaitForSingleObject(NoneActive);
#elif defined(_UNIX)
  AnyActive=true;

  // Threads reset AnyActive before accessing QueuedTasksCnt and even
  // preceding WaitDone() call does not guarantee that some slow thread
  // is not accessing QueuedTasksCnt now. So lock is necessary.
  pthread_mutex_lock(&QueuedTasksCntMutex);
  QueuedTasksCnt+=ActiveThreads;
  pthread_mutex_unlock(&QueuedTasksCntMutex);

  pthread_cond_broadcast(&QueuedTasksCntCond);

  pthread_mutex_lock(&AnyActiveMutex);
  while (AnyActive)
    cpthread_cond_wait(&AnyActiveCond,&AnyActiveMutex);
  pthread_mutex_unlock(&AnyActiveMutex);
#endif
}
#endif // RAR_SMP
