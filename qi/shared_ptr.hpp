/*
 * Copyright (c) 2012 Aldebaran Robotics. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the COPYING file.
 */

#pragma once

#ifndef _LIBQI_QI_SHARED_PTR_HPP_
#define _LIBQI_QI_SHARED_PTR_HPP_

#include <qi/atomic.hpp>
#include <qi/log.hpp>

namespace qi
{
  template <typename T>
  class SharedPtr
  {
  public:
    SharedPtr(T *ptr)
      : _ptr(ptr)
      , _refcount(new qi::Atomic<int>(1))
    {
    }

    ~SharedPtr()
    {
      if (--(*_refcount) == 0)
      {
        delete _ptr;
        delete _refcount;
      }
    }

    SharedPtr(const SharedPtr<T> &sp)
    {
      /*
       * Note that this line is racy.
       * If someone is deleting _refcount,
       * it cannot be used below.
       */
      if (++(*sp._refcount) != 1)
      {
        _ptr = sp._ptr;
        _refcount = sp._refcount;
      }
      else
      {
        qiLogDebug("qi.log.shared_ptr")
                  << "tried to copy a shared pointer targeted for deletion"
                  << std::endl;
      }
    }

    SharedPtr& operator=(SharedPtr<T> &sp)
    {
      // release the current pointer
      if (--(*_refcount) == 0)
      {
        delete _ptr;
        delete _refcount;
      }
      if (++(*sp._refcount) != 1)
      {
        _ptr = sp._ptr;
        _refcount = sp._refcount;
      }
      else
      {
        qiLogDebug("qi.log.shared_ptr")
                  << "tried to copy a shared pointer targeted for deletion"
                  << std::endl;
      }
      return *this;
    }

    T &operator*() const
    {
      return *_ptr;
    }

    T *operator->() const
    {
      return _ptr;
    }

  private:
    T                *_ptr;
    qi::Atomic<int> *_refcount;
  };
}

#endif // _LIBQI_QI_SHARED_PTR_HPP_
