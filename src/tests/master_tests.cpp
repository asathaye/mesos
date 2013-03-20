/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>

#include <stout/error.hpp>
#include <stout/os.hpp>

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "logging/flags.hpp"

#include "flags/flags.hpp"

#include "master/allocator.hpp"
#include "master/flags.hpp"
#include "master/frameworks_manager.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include <process/dispatch.hpp>
#include <process/future.hpp>

#include "slave/constants.hpp"
#include "slave/process_isolator.hpp"
#include "slave/slave.hpp"

#include "tests/filter.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Allocator;
using mesos::internal::master::FrameworksManager;
using mesos::internal::master::FrameworksStorage;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::ProcessIsolator;
using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;

using std::string;
using std::map;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::SaveArg;


class MasterTest : public MesosTest
{};


TEST_F(MasterTest, TaskRunning)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  trigger shutdownCall;

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;
  TaskStatus status;

  trigger resourceOffersCall, statusUpdateCall, resourcesChangedCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(isolator,
              resourcesChanged(_, _, Resources(offers[0].resources())))
    .WillOnce(Trigger(&resourcesChangedCall));

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  WAIT_UNTIL(resourcesChangedCall);

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(MasterTest, ShutdownFrameworkWhileTaskRunning)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  trigger shutdownCall;

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  slaveFlags.executor_shutdown_grace_period = Seconds(0.0);
  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;
  TaskStatus status;

  trigger resourceOffersCall, statusUpdateCall, resourcesChangedCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(isolator,
              resourcesChanged(_, _, Resources(offers[0].resources())))
    .WillOnce(Trigger(&resourcesChangedCall));

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  WAIT_UNTIL(resourcesChangedCall);

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(MasterTest, KillTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  trigger killTaskCall, shutdownCall;

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(Trigger(&killTaskCall));

  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;
  TaskStatus status;

  trigger resourceOffersCall, statusUpdateCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  driver.killTask(taskId);

  WAIT_UNTIL(killTaskCall);

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // To ensure can deallocate MockExecutor.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(MasterTest, StatusUpdateAck)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  trigger shutdownCall;

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  trigger statusUpdateAckMsg;
  EXPECT_MESSAGE(Eq(StatusUpdateAcknowledgementMessage().GetTypeName()),
                 _,
                 Eq(slave))
    .WillOnce(DoAll(Trigger(&statusUpdateAckMsg),
                    Return(false)));


  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;
  TaskStatus status;

  trigger resourceOffersCall, statusUpdateCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  // Ensure the slave gets a status update ACK.
  WAIT_UNTIL(statusUpdateAckMsg);

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(MasterTest, RecoverResources)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  setSlaveResources("cpus:2;mem:1024;disk:1024;ports:[1-10, 20-30]");

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  ExecutorInfo executorInfo;
  executorInfo.MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resources executorResources = Resources::parse(
      "cpus:0.3;mem:200;ports:[5-8, 23-25]");
  executorInfo.mutable_resources()->MergeFrom(executorResources);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers1, offers2, offers3;
  TaskStatus status;

  trigger resourceOffersCall1, resourceOffersCall2, resourceOffersCall3;
  trigger statusUpdateCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers1),
                    Trigger(&resourceOffersCall1)))
    .WillOnce(DoAll(SaveArg<1>(&offers2),
                    Trigger(&resourceOffersCall2)))
    .WillOnce(DoAll(SaveArg<1>(&offers3),
                    Trigger(&resourceOffersCall3)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillRepeatedly(DoAll(SaveArg<1>(&status),
			                    Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall1);

  EXPECT_NE(0u, offers1.size());

  TaskID taskId;
  taskId.set_value("1");

  Resources taskResources = offers1[0].resources() - executorResources;

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers1[0].slave_id());
  task.mutable_resources()->MergeFrom(taskResources);
  task.mutable_executor()->MergeFrom(executorInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offers1[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  trigger killTaskCall;
  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(DoAll(Trigger(&killTaskCall),
                    SendStatusUpdateFromTaskID(TASK_KILLED)));

  driver.killTask(taskId);

  WAIT_UNTIL(killTaskCall);

  // Scheduler should get an offer for task resources.
  WAIT_UNTIL(resourceOffersCall2);

  EXPECT_NE(0u, offers2.size());

  Offer offer = offers2[0];
  EXPECT_EQ(taskResources, offer.resources());

  driver.declineOffer(offer.id());

  // Kill the executor.
  isolator.killExecutor(
      offer.framework_id(), executorInfo.executor_id());

  // Scheduler should get an offer for the complete slave resources.
  WAIT_UNTIL(resourceOffersCall3);

  EXPECT_NE(0u, offers3.size());

  Resources slaveResources = Resources::parse(slaveFlags.resources.get());
  EXPECT_EQ(slaveResources, offers3[0].resources());

  driver.stop();
  driver.join();

  // The mock executor might get a shutdown in this case when the
  // slave exits (since the driver links with the slave).
  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(MasterTest, FrameworkMessage)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  ExecutorDriver* execDriver;
  string execData;

  trigger execFrameworkMessageCall, shutdownCall;

  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(DoAll(SaveArg<1>(&execData),
                    Trigger(&execFrameworkMessageCall)));

  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  // Launch the first (i.e., failing) scheduler and wait until the
  // first status update message is sent to it (drop the message).

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;
  TaskStatus status;
  string schedData;

  trigger resourceOffersCall, statusUpdateCall, schedFrameworkMessageCall;

  EXPECT_CALL(sched, registered(&schedDriver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  EXPECT_CALL(sched, frameworkMessage(&schedDriver, _, _, _))
    .WillOnce(DoAll(SaveArg<3>(&schedData),
                    Trigger(&schedFrameworkMessageCall)));

  schedDriver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  schedDriver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  string hello = "hello";

  schedDriver.sendFrameworkMessage(DEFAULT_EXECUTOR_ID,
                                   offers[0].slave_id(),
                                   hello);

  WAIT_UNTIL(execFrameworkMessageCall);

  EXPECT_EQ(hello, execData);

  string reply = "reply";

  execDriver->sendFrameworkMessage(reply);

  WAIT_UNTIL(schedFrameworkMessageCall);

  EXPECT_EQ(reply, schedData);

  schedDriver.stop();
  schedDriver.join();

  WAIT_UNTIL(shutdownCall); // To ensure can deallocate MockExecutor.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(MasterTest, MultipleExecutors)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec1;
  TaskInfo exec1Task;
  trigger exec1LaunchTaskCall, exec1ShutdownCall;

  EXPECT_CALL(exec1, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<1>(&exec1Task),
                    Trigger(&exec1LaunchTaskCall),
                    SendStatusUpdateFromTask(TASK_RUNNING)));

  EXPECT_CALL(exec1, shutdown(_))
    .WillOnce(Trigger(&exec1ShutdownCall));

  MockExecutor exec2;
  TaskInfo exec2Task;
  trigger exec2LaunchTaskCall, exec2ShutdownCall;

  EXPECT_CALL(exec2, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<1>(&exec2Task),
                    Trigger(&exec2LaunchTaskCall),
                    SendStatusUpdateFromTask(TASK_RUNNING)));

  EXPECT_CALL(exec2, shutdown(_))
    .WillOnce(Trigger(&exec2ShutdownCall));

  ExecutorID executorId1;
  executorId1.set_value("executor-1");

  ExecutorID executorId2;
  executorId2.set_value("executor-2");

  map<ExecutorID, Executor*> execs;
  execs[executorId1] = &exec1;
  execs[executorId2] = &exec2;

  TestingIsolator isolator(execs);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;
  TaskStatus status1, status2;

  trigger resourceOffersCall, statusUpdateCall1, statusUpdateCall2;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status1),
                    Trigger(&statusUpdateCall1)))
    .WillOnce(DoAll(SaveArg<1>(&status2),
                    Trigger(&statusUpdateCall2)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  ASSERT_NE(0u, offers.size());

  ExecutorInfo executor1; // Bug in gcc 4.1.*, must assign on next line.
  executor1 = CREATE_EXECUTOR_INFO(executorId1, "exit 1");

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task1.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:512"));
  task1.mutable_executor()->MergeFrom(executor1);

  ExecutorInfo executor2; // Bug in gcc 4.1.*, must assign on next line.
  executor2 = CREATE_EXECUTOR_INFO(executorId2, "exit 1");

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task2.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:512"));
  task2.mutable_executor()->MergeFrom(executor2);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall1);

  EXPECT_EQ(TASK_RUNNING, status1.state());

  WAIT_UNTIL(statusUpdateCall2);

  EXPECT_EQ(TASK_RUNNING, status2.state());

  WAIT_UNTIL(exec1LaunchTaskCall);

  EXPECT_EQ(task1.task_id(), exec1Task.task_id());

  WAIT_UNTIL(exec2LaunchTaskCall);

  EXPECT_EQ(task2.task_id(), exec2Task.task_id());

  driver.stop();
  driver.join();

  WAIT_UNTIL(exec1ShutdownCall); // To ensure can deallocate MockExecutor.
  WAIT_UNTIL(exec2ShutdownCall); // To ensure can deallocate MockExecutor.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(MasterTest, ShutdownUnregisteredExecutor)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  // Drop the registration message from the executor to the slave.
  trigger registerExecutorMsg;
  EXPECT_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&registerExecutorMsg),
                    Return(true)));

  ProcessIsolator isolator;

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;
  TaskStatus status;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  trigger resourceOffersCall;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  CommandInfo command;
  command.set_value("sleep 10");

  task.mutable_command()->MergeFrom(command);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(registerExecutorMsg);

  Clock::pause();

  // Ensure that the slave times out and kills the executor.
  Clock::advance(slaveFlags.executor_registration_timeout.secs());
  Clock::settle();

  // Ensure that the reaper reaps the executor.
  Clock::advance(1.0);
  Clock::settle();

  WAIT_UNTIL(statusUpdateCall);

  // This signals that the command executor has exited.
  ASSERT_EQ(TASK_FAILED, status.state());

  Clock::resume();

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(MasterTest, MasterInfo)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  MasterInfo masterInfo;

  trigger registeredCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(DoAll(SaveArg<2>(&masterInfo),
                    Trigger(&registeredCall)));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(registeredCall);

  EXPECT_EQ(master.port, masterInfo.port());
  EXPECT_EQ(master.ip, masterInfo.ip());

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(MasterTest, MasterInfoOnReElection)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  MasterInfo masterInfo;

  trigger registeredCall, reregisteredCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(Trigger(&registeredCall));

  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&masterInfo),
                    Trigger(&reregisteredCall)));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  process::Message message;

  EXPECT_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(SaveArgField<0>(&process::MessageEvent::message, &message),
                    Return(false)));

  driver.start();

  WAIT_UNTIL(registeredCall);

  // Simulate a spurious newMasterDetected event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  NewMasterDetectedMessage newMasterDetectedMsg;
  newMasterDetectedMsg.set_pid(master);

  process::post(message.to, newMasterDetectedMsg);

  WAIT_UNTIL(reregisteredCall);

  EXPECT_EQ(master.port, masterInfo.port());
  EXPECT_EQ(master.ip, masterInfo.ip());

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


class WhitelistTest : public MasterTest
{
protected:
  WhitelistTest()
    : path("whitelist.txt")
  {}

  virtual ~WhitelistTest()
  {
    os::rm(path);
  }
  const string path;
};


TEST_F(WhitelistTest, WhitelistSlave)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  // Add some hosts to the white list.
  Try<string> hostname = os::hostname();
  ASSERT_SOME(hostname);
  string hosts = hostname.get() + "\n" + "dummy-slave";
  ASSERT_SOME(os::write(path, hosts)) << "Error writing whitelist";

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  master::Flags masterFlags;
  masterFlags.whitelist = "file://" + path;
  Master m(&a, &files, masterFlags);
  PID<Master> master = process::spawn(&m);

  trigger slaveRegisteredMsg;

  EXPECT_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&slaveRegisteredMsg),
                    Return(false)));

  MockExecutor exec;

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  MasterInfo masterInfo;

  trigger registeredCall, reregisteredCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(Trigger(&registeredCall));

  trigger resourceOffersCall;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(Trigger(&resourceOffersCall));

  driver.start();

  WAIT_UNTIL(slaveRegisteredMsg);

  WAIT_UNTIL(resourceOffersCall);

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


// FrameworksManager test cases.

class MockFrameworksStorage : public FrameworksStorage
{
public:
  // We need this typedef because MOCK_METHOD is a macro.
  typedef map<FrameworkID, FrameworkInfo> Map_FrameworkId_FrameworkInfo;

  MOCK_METHOD0(list, Future<Result<Map_FrameworkId_FrameworkInfo> >());
  MOCK_METHOD2(add, Future<Result<bool> >(const FrameworkID&,
                                          const FrameworkInfo&));
  MOCK_METHOD1(remove, Future<Result<bool> >(const FrameworkID&));
};


TEST_F(MasterTest, MasterLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  MasterInfo masterInfo;

  trigger registeredCall, disconnectedCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(Trigger(&registeredCall));

  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(Trigger(&disconnectedCall));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  process::Message message;

  EXPECT_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(SaveArgField<0>(&process::MessageEvent::message, &message),
                    Return(false)));

  driver.start();

  WAIT_UNTIL(registeredCall);

  // Simulate a spurious noMasterDetected event at the scheduler.
  NoMasterDetectedMessage noMasterDetectedMsg;
  process::post(message.to, noMasterDetectedMsg);

  WAIT_UNTIL(disconnectedCall);

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}

// This fixture sets up expectations on the storage class
// and spawns both storage and frameworks manager.
class FrameworksManagerTestFixture : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    ASSERT_TRUE(GTEST_IS_THREADSAFE);

    storage = new MockFrameworksStorage();
    process::spawn(storage);

    EXPECT_CALL(*storage, list())
      .WillOnce(Return(Result<map<FrameworkID, FrameworkInfo> >(infos)));

    EXPECT_CALL(*storage, add(_, _))
      .WillRepeatedly(Return(Result<bool>::some(true)));

    EXPECT_CALL(*storage, remove(_))
      .WillRepeatedly(Return(Result<bool>::some(true)));

    manager = new FrameworksManager(storage);
    process::spawn(manager);
  }

  virtual void TearDown()
  {
    process::terminate(manager);
    process::wait(manager);
    delete manager;

    process::terminate(storage);
    process::wait(storage);
    delete storage;
  }

  map<FrameworkID, FrameworkInfo> infos;

  MockFrameworksStorage* storage;
  FrameworksManager* manager;
};


TEST_F(FrameworksManagerTestFixture, AddFramework)
{
  // Test if initially FM returns empty list.
  Future<Result<map<FrameworkID, FrameworkInfo> > > future =
    process::dispatch(manager, &FrameworksManager::list);

  ASSERT_TRUE(future.await(Seconds(2.0)));
  EXPECT_TRUE(future.get().get().empty());

  // Add a dummy framework.
  FrameworkID id;
  id.set_value("id");

  FrameworkInfo info;
  info.set_name("test name");
  info.set_user("test user");

  // Add the framework.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::add, id, info);

  ASSERT_TRUE(future2.await(Seconds(2.0)));
  EXPECT_TRUE(future2.get().get());

  // Check if framework manager returns the added framework.
  Future<Result<map<FrameworkID, FrameworkInfo> > > future3 =
    process::dispatch(manager, &FrameworksManager::list);

  ASSERT_TRUE(future3.await(Seconds(2.0)));

  map<FrameworkID, FrameworkInfo> result = future3.get().get();

  ASSERT_EQ(1u, result.count(id));
  EXPECT_EQ("test name", result[id].name());
  EXPECT_EQ("test user", result[id].user());

  // Check if the framework exists.
  Future<Result<bool> > future4 =
    process::dispatch(manager, &FrameworksManager::exists, id);

  ASSERT_TRUE(future4.await(Seconds(2.0)));
  EXPECT_TRUE(future4.get().get());
}


TEST_F(FrameworksManagerTestFixture, RemoveFramework)
{
  Clock::pause();

  // Remove a non-existent framework.
  FrameworkID id;
  id.set_value("non-existent framework");

  Future<Result<bool> > future1 =
    process::dispatch(manager, &FrameworksManager::remove, id, Seconds(0));

  ASSERT_TRUE(future1.await(Seconds(2.0)));
  EXPECT_TRUE(future1.get().isError());

  // Remove an existing framework.

  // First add a dummy framework.
  FrameworkID id2;
  id2.set_value("id2");

  FrameworkInfo info2;
  info2.set_name("test name");
  info2.set_user("test user");

  // Add the framework.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::add, id2, info2);

  ASSERT_TRUE(future2.await(Seconds(2.0)));
  EXPECT_TRUE(future2.get().get());

  // Now remove the added framework.
  Future<Result<bool> > future3 =
    process::dispatch(manager, &FrameworksManager::remove, id2, Seconds(1.0));

  Clock::update(Clock::now(manager) + 1.0);

  ASSERT_TRUE(future3.await(Seconds(2.0)));
  EXPECT_TRUE(future2.get().get());

  // Now check if the removed framework exists...it shouldn't.
  Future<Result<bool> > future4 =
    process::dispatch(manager, &FrameworksManager::exists, id2);

  ASSERT_TRUE(future4.await(Seconds(2.0)));
  EXPECT_FALSE(future4.get().get());

  Clock::resume();
}


TEST_F(FrameworksManagerTestFixture, ResurrectFramework)
{
  // Resurrect a non-existent framework.
  FrameworkID id;
  id.set_value("non-existent framework");

  Future<Result<bool> > future1 =
    process::dispatch(manager, &FrameworksManager::resurrect, id);

  ASSERT_TRUE(future1.await(Seconds(2.0)));
  EXPECT_FALSE(future1.get().get());

  // Resurrect an existent framework that is NOT being removed.
  // Add a dummy framework.
  FrameworkID id2;
  id2.set_value("id2");

  FrameworkInfo info2;
  info2.set_name("test name");
  info2.set_user("test user");

  // Add the framework.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::add, id2, info2);

  ASSERT_TRUE(future2.await(Seconds(2.0)));
  EXPECT_TRUE(future2.get().get());

  Future<Result<bool> > future3 =
    process::dispatch(manager, &FrameworksManager::resurrect, id2);

  ASSERT_TRUE(future3.await(Seconds(2.0)));
  EXPECT_TRUE(future3.get().get());
}


// TODO(vinod): Using a paused clock in the tests means that
// future.await() may wait forever. This makes debugging hard.
TEST_F(FrameworksManagerTestFixture, ResurrectExpiringFramework)
{
  // This is the crucial test.
  // Resurrect an existing framework that is being removed,is being removed,
  // which should cause the remove to be unsuccessful.

  // Add a dummy framework.
  FrameworkID id;
  id.set_value("id");

  FrameworkInfo info;
  info.set_name("test name");
  info.set_user("test user");

  // Add the framework.
  process::dispatch(manager, &FrameworksManager::add, id, info);

  Clock::pause();

  // Remove after 2 secs.
  Future<Result<bool> > future1 =
    process::dispatch(manager, &FrameworksManager::remove, id, Seconds(2.0));

  // Resurrect in the meanwhile.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::resurrect, id);

  ASSERT_TRUE(future2.await(Seconds(2.0)));
  EXPECT_TRUE(future2.get().get());

  Clock::update(Clock::now(manager) + 2.0);

  ASSERT_TRUE(future1.await(Seconds(2.0)));
  EXPECT_FALSE(future1.get().get());

  Clock::resume();
}


TEST_F(FrameworksManagerTestFixture, ResurrectInterspersedExpiringFrameworks)
{
  // This is another crucial test.
  // Two remove messages are interspersed with a resurrect.
  // Only the second remove should actually remove the framework.

  // Add a dummy framework.
  FrameworkID id;
  id.set_value("id");

  FrameworkInfo info;
  info.set_name("test name");
  info.set_user("test user");

  // Add the framework.
  process::dispatch(manager, &FrameworksManager::add, id, info);

  Clock::pause();

  Future<Result<bool> > future1 =
    process::dispatch(manager, &FrameworksManager::remove, id, Seconds(2.0));

  // Resurrect in the meanwhile.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::resurrect, id);

  // Remove again.
  Future<Result<bool> > future3 =
    process::dispatch(manager, &FrameworksManager::remove, id, Seconds(1.0));

  ASSERT_TRUE(future2.await(Seconds(2.0)));
  EXPECT_TRUE(future2.get().get());

  Clock::update(Clock::now(manager) + 1.0);

  ASSERT_TRUE(future3.await(Seconds(2.0)));
  EXPECT_TRUE(future3.get().get());

  Clock::update(Clock::now(manager) + 2.0);

  ASSERT_TRUE(future1.await(Seconds(2.0)));
  EXPECT_FALSE(future1.get().get());

  Clock::resume();
}


// Not deriving from fixture...because we want to set specific expectations.
// Specifically we simulate caching failure in FrameworksManager.
TEST(FrameworksManagerTest, CacheFailure)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFrameworksStorage storage;
  process::spawn(storage);

  EXPECT_CALL(storage, list())
    .Times(2)
    .WillRepeatedly(Return(Error("Fake Caching Error")));

  EXPECT_CALL(storage, add(_, _))
    .WillOnce(Return(Result<bool>::some(true)));

  EXPECT_CALL(storage, remove(_))
    .Times(0);

  FrameworksManager manager(&storage);
  process::spawn(manager);

  // Test if initially FrameworksManager returns error.
  Future<Result<map<FrameworkID, FrameworkInfo> > > future1 =
    process::dispatch(manager, &FrameworksManager::list);

  ASSERT_TRUE(future1.await(Seconds(2.0)));
  ASSERT_TRUE(future1.get().isError());
  EXPECT_EQ(future1.get().error(), "Error caching framework infos");

  // Add framework should function normally despite caching failure.
  FrameworkID id;
  id.set_value("id");

  FrameworkInfo info;
  info.set_name("test name");
  info.set_user("test user");

  // Add the framework.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::add, id, info);

  ASSERT_TRUE(future2.await(Seconds(2.0)));
  EXPECT_TRUE(future2.get().get());

  // Remove framework should fail due to caching failure.
  Future<Result<bool> > future3 =
    process::dispatch(manager, &FrameworksManager::remove, id, Seconds(0));

  ASSERT_TRUE(future3.await(Seconds(2.0)));
  ASSERT_TRUE(future3.get().isError());
  EXPECT_EQ(future3.get().error(), "Error caching framework infos");

  process::terminate(manager);
  process::wait(manager);

  process::terminate(storage);
  process::wait(storage);
}
