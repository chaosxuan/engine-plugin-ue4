// Copyright (C) 2013-2016 iFunFactory Inc. All Rights Reserved.
//
// This work is confidential and proprietary to iFunFactory Inc. and
// must not be used, disclosed, copied, or distributed without the prior
// consent of iFunFactory Inc.

#ifndef SRC_FUNAPI_TASKS_H_
#define SRC_FUNAPI_TASKS_H_

#include "funapi_build_config.h"

#include <memory>
#include <functional>
#include <string>

namespace fun {

class FunapiTasksImpl;
class FUNAPI_API FunapiTasks : public std::enable_shared_from_this<FunapiTasks> {
 public:
  typedef std::function<bool()> TaskHandler;

  FunapiTasks();
  virtual ~FunapiTasks();

  static std::shared_ptr<FunapiTasks> Create();
  static void UpdateAll();

  void Set(const TaskHandler &task);
  void Push(const TaskHandler &task);
  void Update();

 private:
  std::weak_ptr<FunapiTasksImpl> impl_;
};


class FunapiThreadImpl;
class FUNAPI_API FunapiThread : public std::enable_shared_from_this<FunapiThread> {
 public:
  typedef FunapiTasks::TaskHandler TaskHandler;

  FunapiThread() = delete;
  FunapiThread(const std::string &thread_id_string, const TaskHandler &task);
  virtual ~FunapiThread();

  static std::shared_ptr<FunapiThread> Create(const std::string &thread_id_string, const TaskHandler &task);
  static std::shared_ptr<FunapiThread> Get(const std::string &thread_id_string);

  static bool Set(const std::string &thread_id_string, const TaskHandler &task);
  static bool Push(const std::string &thread_id_string, const TaskHandler &task);
  static bool Join(const std::string &thread_id_string);

  void Set(const TaskHandler &task);
  void Push(const TaskHandler &task);
  void Join();

 private:
  std::weak_ptr<FunapiThreadImpl> impl_;
};

}  // namespace fun

#endif  // SRC_FUNAPI_TASKS_H_
