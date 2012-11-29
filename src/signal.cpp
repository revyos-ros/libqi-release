/*
**  Copyright (C) 2012 Aldebaran Robotics
**  See COPYING for the license
*/
#include <map>
#include <qi/atomic.hpp>

#include <boost/thread/recursive_mutex.hpp>
#include <boost/make_shared.hpp>

#include <qitype/signal.hpp>
#include <qitype/genericvalue.hpp>
#include <qitype/genericobject.hpp>

#include "object_p.hpp"
#include "signal_p.hpp"

namespace qi {

  SignalSubscriber::SignalSubscriber(qi::ObjectPtr target, unsigned int method)
  : weakLock(0), target(new qi::ObjectWeakPtr(target)), method(method), enabled(true)
  {}



  SignalSubscriber::SignalSubscriber(GenericFunction func, EventLoop* ctx, detail::WeakLock* lock)
     : handler(func), weakLock(lock), target(0), method(0), enabled(true)
   {
     eventLoopGetter = boost::bind(detail::eventLoopGet, ctx);
   }

  SignalSubscriber::~SignalSubscriber()
  {
    delete target;
    delete weakLock;
  }

  SignalSubscriber::SignalSubscriber(const SignalSubscriber& b)
  : weakLock(0), target(0)
  {
    *this = b;
  }

  void SignalSubscriber::operator=(const SignalSubscriber& b)
  {
    source = b.source;
    linkId = b.linkId;
    handler = b.handler;
    weakLock = b.weakLock?b.weakLock->clone():0;
    eventLoopGetter = b.eventLoopGetter;
    target = b.target?new ObjectWeakPtr(*b.target):0;
    method = b.method;
    enabled = b.enabled;
  }

  static qi::Atomic<int> linkUid = 1;

  void SignalBase::operator()(
      qi::AutoGenericValuePtr p1,
      qi::AutoGenericValuePtr p2,
      qi::AutoGenericValuePtr p3,
      qi::AutoGenericValuePtr p4,
      qi::AutoGenericValuePtr p5,
      qi::AutoGenericValuePtr p6,
      qi::AutoGenericValuePtr p7,
      qi::AutoGenericValuePtr p8)
  {
    qi::AutoGenericValuePtr* vals[8]= {&p1, &p2, &p3, &p4, &p5, &p6, &p7, &p8};
    std::vector<qi::GenericValuePtr> params;
    for (unsigned i = 0; i < 8; ++i)
      if (vals[i]->value)
        params.push_back(*vals[i]);
    // Signature construction
    std::string signature = "(";
    for (unsigned i=0; i< params.size(); ++i)
      signature += params[i].signature();
    signature += ")";
    if (signature != _p->signature)
    {
      qiLogError("qi.signal") << "Dropping emit: signature mismatch: " << signature <<" " << _p->signature;
      return;
    }
    trigger(params);
  }

  void SignalBase::trigger(const GenericFunctionParameters& params)
  {
    if (!_p)
      return;

    SignalSubscriberMap copy;
    {
      boost::recursive_mutex::scoped_lock sl(_p->mutex);
      copy = _p->subscriberMap;
    }

    SignalSubscriberMap::iterator i;
    for (i = copy.begin(); i != copy.end(); ++i)
    {
      SignalSubscriberPtr s = i->second; // hold s alive
      s->call(params);
    }
  }

  class FunctorCall
  {
  public:
    FunctorCall(GenericFunctionParameters& params, SignalSubscriberPtr sub)
    : sub(sub)
    {
      std::swap((std::vector<GenericValuePtr>&)this->params, (std::vector<GenericValuePtr>&)params);
    }

    FunctorCall(const FunctorCall& b)
    {
      *this = b;
    }

    void operator=(const FunctorCall& b)
    {
      std::swap((std::vector<GenericValuePtr>&)(this->params),
        (std::vector<GenericValuePtr>&)b.params);
      sub = b.sub;
    }

    void operator() ()
    {
      {
        boost::mutex::scoped_lock sl(sub->mutex);
        // verify-enabled-then-register-active op must be locked
        if (!sub->enabled)
          return;
        sub->addActive(false);
      }
      sub->handler(params);
      sub->removeActive(true);
      params.destroy();
      if (sub->weakLock)
        sub->weakLock->unlock();
    }

  public:
    GenericFunctionParameters params;
    SignalSubscriberPtr         sub;
  };

  void SignalSubscriber::call(const GenericFunctionParameters& args)
  {
    // this is held alive by caller
    if (handler.type)
    {
      // Try to acquire weakLock, for both the sync and async cases
      if (weakLock)
      {
        bool locked = weakLock->tryLock();
        if (!locked)
        {
          source->disconnect(linkId);
          return;
        }
      }
      EventLoop* eventLoop = 0;
      if (eventLoopGetter)
        eventLoop = eventLoopGetter();
      if (eventLoop)
      {
        // Event emission is always asynchronous
        GenericFunctionParameters copy = args.copy();
        // We will check enabled when we will be scheduled in the target
        // thread, and we hold this SignalSubscriber alive, so no need to
        // explicitly track the asynccall
        eventLoop->asyncCall(0, FunctorCall(copy, shared_from_this()));
      }
      else
      {
        // verify-enabled-then-register-active op must be locked
        {
          boost::mutex::scoped_lock sl(mutex);
          if (!enabled)
            return;
          addActive(false);
        }
        handler(args);
        if (weakLock)
          weakLock->unlock();
        removeActive(true);
      }
    }
    else if (target)
    {
      ObjectPtr lockedTarget = target->lock();
      if (!lockedTarget)
      {
        source->disconnect(linkId);
      }
      else // no need to keep anything locked, whatever happens this is not used
        lockedTarget->metaPost(method, args);
    }
  }

  //check if we are called from the same thread that triggered us.
  //in that case, do not wait.
  void SignalSubscriber::waitForInactive()
  {
    boost::thread::id tid = boost::this_thread::get_id();
    while (true)
    {
      {
        boost::mutex::scoped_lock sl(mutex);
        if (activeThreads.empty())
          return;
        // There cannot be two activeThreads entry for the same tid
        // because activeThreads is not set at the post() stage
        if (activeThreads.size() == 1
          && *activeThreads.begin() == tid)
        { // One active callback in this thread, means above us in call stack
          // So we cannot wait for it
          return;
        }
      }
      os::msleep(1); // FIXME too long use a condition
    }
  }

  void SignalSubscriber::addActive(bool acquireLock, boost::thread::id id)
  {
    if (acquireLock)
    {
      boost::mutex::scoped_lock sl(mutex);
      activeThreads.push_back(id);
    }
    else
      activeThreads.push_back(id);
  }

  void SignalSubscriber::removeActive(bool acquireLock, boost::thread::id id)
  {

    boost::mutex::scoped_lock sl(mutex, boost::defer_lock_t());
    if (acquireLock)
      sl.lock();

    for (unsigned i=0; i<activeThreads.size(); ++i)
    {
      if (activeThreads[i] == id)
      { // fast remove by swapping with last and then pop_back
        activeThreads[i] = activeThreads[activeThreads.size() - 1];
        activeThreads.pop_back();
      }
    }
  }

  SignalBase::Link SignalBase::connect(GenericFunction callback, EventLoop* ctx)
  {
    return connect(SignalSubscriber(callback, ctx));
  }

  SignalBase::Link SignalBase::connect(qi::ObjectPtr o, unsigned int slot)
  {
    return connect(SignalSubscriber(o, slot));
  }

  SignalBase::Link SignalBase::connect(const SignalSubscriber& src)
  {
    if (!_p)
    {
      _p = boost::shared_ptr<SignalBasePrivate>(new SignalBasePrivate());
    }
    // Check arity. Does not require to acquire weakLock.
    int sigArity = Signature(signature()).begin().children().size();
    int subArity = -1;
    if (src.handler.type)
    {
      if (src.handler.type == dynamicFunctionType())
        goto proceed; // no arity checking is possible
      subArity = src.handler.type->argumentsType().size();
    }
    else if (src.target)
    {
      ObjectPtr locked = src.target->lock();
      if (!locked)
      {
        qiLogVerbose("qi.signal") << "connecting a dead slot (weak ptr out)";
        return SignalBase::invalidLink;
      }
      const MetaMethod* ms = locked->metaObject().method(src.method);
      if (!ms)
      {
        qiLogWarning("qi.signal") << "Method " << src.method <<" not found, proceeding anyway";
        goto proceed;
      }
      else
        subArity = Signature(qi::signatureSplit(ms->signature())[2]).size();
    }
    if (sigArity != subArity)
    {
      qiLogWarning("qi.signal") << "Subscriber has incorrect arity (expected "
        << sigArity  << " , got " << subArity <<")";
      return SignalBase::invalidLink;
    }
  proceed:
    boost::recursive_mutex::scoped_lock sl(_p->mutex);
    Link res = ++linkUid;
    SignalSubscriberPtr s = boost::make_shared<SignalSubscriber>(src);
    s->linkId = res;
    s->source = this;
    _p->subscriberMap[res] = s;
    return res;
  }

  bool SignalBase::disconnectAll() {
    if (_p)
      return _p->reset();
    return false;
  }

  SignalBase::SignalBase(const std::string& sig)
    : _p(new SignalBasePrivate)
  {
    _p->signature = sig;
  }

  SignalBase::SignalBase()
  {
  }

  SignalBase::SignalBase(const SignalBase& b)
  {
    (*this) = b;
  }

  SignalBase& SignalBase::operator=(const SignalBase& b)
  {
    if (!b._p)
    {
      const_cast<SignalBase&>(b)._p = boost::shared_ptr<SignalBasePrivate>(new SignalBasePrivate());
    }
    _p = b._p;
    return *this;
  }

  std::string SignalBase::signature() const
  {
    return _p ? _p->signature : "";
  }

  bool SignalBasePrivate::disconnect(const SignalBase::Link& l)
  {

    SignalSubscriberPtr s;
    // Acquire signal mutex
    boost::recursive_mutex::scoped_lock sigLock(mutex);
    SignalSubscriberMap::iterator it = subscriberMap.find(l);
    if (it == subscriberMap.end())
      return false;
    s = it->second;
    // Remove from map (but SignalSubscriber object still good)
    subscriberMap.erase(it);
    // Acquire subscriber mutex before releasing mutex
    boost::mutex::scoped_lock subLock(s->mutex);
    // Release signal mutex
    sigLock.release()->unlock();
    // Ensure no call on subscriber occurrs once this function returns
    s->enabled = false;

    if ( s->activeThreads.empty()
         || (s->activeThreads.size() == 1
             && *s->activeThreads.begin() == boost::this_thread::get_id()))
    { // One active callback in this thread, means above us in call stack
      // So we cannot trash s right now
      return true;
    }
    // More than one active callback, or one in a state that prevent us
    // from knowing in which thread it will run
    subLock.release()->unlock();
    s->waitForInactive();
    return true;
  }

  bool SignalBase::disconnect(const Link &link) {
    if (!_p)
      return false;
    else
      return _p->disconnect(link);
  }

  SignalBase::~SignalBase()
  {
    if (!_p)
      return;
    SignalSubscriberMap::iterator i;
    std::vector<Link> links;
    for (i = _p->subscriberMap.begin(); i!= _p->subscriberMap.end(); ++i)
    {
      links.push_back(i->first);
    }
    for (unsigned i=0; i<links.size(); ++i)
      disconnect(links[i]);
  }

  std::vector<SignalSubscriber> SignalBase::subscribers()
  {
    std::vector<SignalSubscriber> res;
    if (!_p)
      return res;
    boost::recursive_mutex::scoped_lock sl(_p->mutex);
    SignalSubscriberMap::iterator i;
    for (i = _p->subscriberMap.begin(); i!= _p->subscriberMap.end(); ++i)
      res.push_back(*i->second);
    return res;
  }

  bool SignalBasePrivate::reset() {
    bool ret = true;
    boost::recursive_mutex::scoped_lock sl(mutex);
    SignalSubscriberMap::iterator it = subscriberMap.begin();
    while (it != subscriberMap.end()) {
      bool b = disconnect(it->first);
      if (!b)
        ret = false;
      it = subscriberMap.begin();
    }
    return ret;
  }

  QITYPE_API const SignalBase::Link SignalBase::invalidLink = ((unsigned int)-1);

}
