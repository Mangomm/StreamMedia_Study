#include <iostream>
#include <thread>
#include <condition_variable>
#include <mutex>
using namespace std;

int gData = 0;
mutex mut;
condition_variable cond;

void thread1() {
	if (gData >= 1) {
		unique_lock<mutex> uni(mut);
		std::cout << "读线程拿到锁" << std::endl;
		cond.wait_for(uni, std::chrono::seconds(2));
		std::cout << "读线程超时退出了" << std::endl;
		uni.unlock();
	}
}

void thread2() {
	// 1. 目的：先睡让读线程拿锁，然后读线程阻塞在条件变量，此时我们就能拿到锁，然后再睡眠超过读线程的2s以上。看看是否能超时返回。

	// 2. 验证结果：虽然读线程超时2s想退出，但是我写线程卡主2s以上，让你拿不到锁无法退出。
	//		所以读写线程中，一个条件变量配合一把锁，如果你一个线程是使用了超时返回，但另一个线程一直占用着锁的话，它也是无法按时超时返回的，
	//		这样就会导致这个线程无法处理其它的事情。

	// 3. 解决的方法是：参考ffplay，1）队列出入单独使用一把锁(mutex1)+条件变量(cond1)；2）为满时使用一把局部变量锁(mutex2)+条件变
	//		量(cond2)等待超时(读线程的任务，因为为满只需要读线程处理，读线程不需要处理空的情况)，
	//		3）为空时只需要notify条件变量(cond2)即可，完全不需要用到锁(写线程的任务，因为为空没东西消费，需要通知读线程，有东西当然不需要通知)。
	//	即ffpaly将队列的出入与为满为空的情况进行分别处理，以减少锁的粒度。
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	unique_lock<mutex> uni(mut);
	std::cout << "写线程拿到锁啦" << std::endl;
	time_t start = time(NULL);
	std::this_thread::sleep_for(std::chrono::milliseconds(3000));
	time_t end = time(NULL);
	std::cout << "虽然读线程超时2s想退出，但是我写线程卡主2s以上，让你拿不到锁无法退出, 卡主时间: " << end - start << std::endl;
	uni.unlock();
	std::cout << "写线程解锁锁啦" << std::endl;

}

int main() {

	gData = 1;
	thread write(thread2);
	thread read(thread1);
	
	while (1) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

	read.join();
	write.join();

	return 0;
}