/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <thread>
#include <vector>

#include <boost/noncopyable.hpp>

#include <osquery/core.h>

namespace osquery {

/// A throw/catch relay between a pause request and cancel event.
struct RunnerInterruptError {};

class RunnerInterruptPoint : private boost::noncopyable {
 public:
  RunnerInterruptPoint() : stop_(false) {}

  /// Cancel the pause request.
  void cancel();

  /// Pause until the requested millisecond delay has elapsed or a cancel.
  void pause(size_t milli);

 private:
  /// Communicate between the pause and cancel event.
  bool stop_;

  /// Protection around pause and cancel calls.
  std::mutex mutex_;

  /// Wait for notification or a pause expiration.
  std::condition_variable condition_;
};

class InternalRunnable : private boost::noncopyable {
 public:
  InternalRunnable() : run_(false) {}
  virtual ~InternalRunnable() {}

 public:
  /**
   * @brief The std::thread entrypoint.
   *
   * This is used by the Dispatcher only.
   */
  virtual void run() final {
    run_ = true;
    start();
  }

  /**
   * @brief Check if the thread's entrypoint (run) executed.
   *
   * It is possible for the Runnable to be allocated without the thread context.
   * ::hasRun makes a much better guess at the state of the thread.
   * If it has run then stop must be called.
   */
  bool hasRun() { return run_; }

  /**
   * @brief The std::thread's interruption point.
   */
  virtual void interrupt() final {
    WriteLock lock(stopping_);
    // Set the service as interrupted.
    interrupted_ = true;
    // Tear down the service's resources such that exiting the expected run
    // loop within ::start does not need to.
    stop();
    // Cancel the run loop's pause request.
    point_.cancel();
  }

 protected:
  /// Allow the runnable to check interruption.
  bool interrupted() {
    WriteLock lock(stopping_);
    return interrupted_;
  }

  /// Require the runnable thread define an entrypoint.
  virtual void start() = 0;

  /// Require the runnable thread to define a stop/interrupt point.
  virtual void stop() {}

  /// Put the runnable into an interruptible sleep.
  virtual void pause() { pauseMilli(100); }

  /// Put the runnable into an interruptible sleep.
  virtual void pauseMilli(size_t milli);

 private:
  std::atomic<bool> run_{false};

  /// If a service includes a run loop it should check for interrupted.
  std::atomic<bool> interrupted_{false};

  /**
   * @brief Protect interruption checking and resource tear down.
   *
   * A tearDown mutex protects the runnable service's resources.
   * Interruption means resources have been stopped.
   * Non-interruption means no attempt to affect resources has been started.
   */
  std::mutex stopping_;

  /// Use an interruption point to exit a pause if the thread was interrupted.
  RunnerInterruptPoint point_;
};

/// An internal runnable used throughout osquery as dispatcher services.
using InternalRunnableRef = std::shared_ptr<InternalRunnable>;
using InternalThreadRef = std::shared_ptr<std::thread>;

/**
 * @brief Singleton for queuing asynchronous tasks to be executed in parallel
 *
 * Dispatcher is a singleton which can be used to coordinate the parallel
 * execution of asynchronous tasks across an application. Internally,
 * Dispatcher is back by the Apache Thrift thread pool.
 */
class Dispatcher : private boost::noncopyable {
 public:
  /**
   * @brief The primary way to access the Dispatcher factory facility.
   *
   * @code{.cpp} auto dispatch = osquery::Dispatcher::instance(); @endcode
   *
   * @return The osquery::Dispatcher instance.
   */
  static Dispatcher& instance() {
    static Dispatcher instance;
    return instance;
  }

  /// See `add`, but services are not limited to a thread poll size.
  static Status addService(InternalRunnableRef service);

  /// See `join`, but applied to osquery services.
  static void joinServices();

  /// Destroy and stop all osquery service threads and service objects.
  static void stopServices();

  /// Return number of services.
  size_t serviceCount() { return service_threads_.size(); }

 private:
  /**
   * @brief Default constructor.
   *
   * Since instances of Dispatcher should only be created via instance(),
   * Dispatcher's constructor is private.
   */
  Dispatcher() {}
  Dispatcher(Dispatcher const&);
  void operator=(Dispatcher const&);
  virtual ~Dispatcher() {}

 private:
  /// The set of shared osquery service threads.
  std::vector<InternalThreadRef> service_threads_;

  /// The set of shared osquery services.
  std::vector<InternalRunnableRef> services_;

 private:
  friend class ExtensionsTest;
};
}
