#include <gtest/gtest.h>

#include <qi/qi.hpp>
#include <qi/application.hpp>
#include <qitype/genericobject.hpp>
#include <qitype/genericobjectbuilder.hpp>
#include <qitype/objectfactory.hpp>
#include <qimessaging/session.hpp>
#include <testsession/testsessionpair.hpp>
#include <qimessaging/servicedirectory.hpp>


#include "tests/task_proxy.hpp"


class TestTask: public ::testing::Test
{
public:
  TestTask()
  {
    static bool init = false;
    static qi::ObjectPtr tgs;
    if (!init)
    {
      std::vector<std::string> objs = qi::loadObject("taskservice");
      if (objs.size() != 1)
        throw std::runtime_error("Expected one object in taskService");
      tgs = qi::createObject("TaskGeneratorService");
      if (!tgs)
        throw std::runtime_error("No TaskGenerator service found");
      init = true;
    }
    taskGenService = tgs;
  }

protected:
  void SetUp()
  {
    p.server()->registerService("taskGen", taskGenService);
    taskGenClient = p.client()->service("taskGen");
    ASSERT_TRUE(taskGenClient);
    taskGenProxy = new TaskGeneratorProxy(taskGenClient);
  }
  
  void TearDown()
  {
    taskGenClient.reset();
    delete taskGenProxy;
  }

public:
  TestSessionPair      p;
  qi::ObjectPtr        taskGenService;
  qi::ObjectPtr        taskGenClient;
  TaskGeneratorProxy*  taskGenProxy; // specialized version of taskClient
};

TEST_F(TestTask, Basic)
{
  ASSERT_EQ(0, taskGenProxy->taskCount());
}
TEST_F(TestTask, Task)
{
  ITaskPtr task = taskGenProxy->newTask("coin");
  ASSERT_TRUE(task);
}

int main(int argc, char **argv) {
  qi::Application app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  TestMode::forceTestMode(TestMode::Mode_SD);
  TestMode::initTestMode(argc, argv);
  return RUN_ALL_TESTS();
}
