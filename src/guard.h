#ifndef _GUARD_H_
#define _GUARD_H_

#include <mutex>
#include <memory>
#include <functional>

namespace atomic{
    template<typename T>
    class guard {
    public:
        std::mutex shared; // Mutex to guard shared data
        std::unique_ptr<T> data;

        /**
         * @brief Constructs a Guard object and initializes its Data member.
         * 
         * This constructor creates a unique pointer to an instance of type T
         * and assigns it to the Data member of the Guard object.
         */
        guard() : data(std::make_unique<T>()) {}

        /**
         * @brief Functor to execute a job with thread safety.
         * 
         * This operator() function takes a std::function that operates on a reference to a T object.
         * It ensures that the job is executed with mutual exclusion by using a std::lock_guard to lock
         * the mutex. If the job throws an exception, it catches it and reports the failure.
         * 
         * @param job A std::function that takes a reference to a T object and performs some operation.
         * 
         * @throws Any exception thrown by the job function will be caught and reported.
         */
        void operator()(std::function<void(T&)> job) {
            std::lock_guard<std::mutex> lock(shared); // Automatically manages mutex locking and unlocking
            try {
                job(*data);
            } catch (const std::exception& e) {
                perror(e.what());
            } catch (...) {
                perror("Unknown exception occurred in job execution!");
            }
        }

        /**
         * @brief Reads the data in a thread-safe manner.
         * 
         * This function acquires a lock on the shared mutex to ensure that the data
         * is read in a thread-safe manner. It returns a copy of the data.
         * 
         * @return T A copy of the data.
         */
        T read() {
            std::lock_guard<std::mutex> lock(shared);
            return *data;
        }

        /**
         * @brief Destructor for the Guard class.
         *
         * This destructor ensures that the Data object is properly destroyed
         * by acquiring a lock on the Shared mutex before resetting the Data.
         * The use of std::lock_guard ensures that the mutex is automatically
         * released when the destructor exits, preventing potential deadlocks.
         */
        ~guard() {
            std::lock_guard<std::mutex> lock(shared);
            data.reset(); // Ensures proper destruction
        }
    };   
}

#endif