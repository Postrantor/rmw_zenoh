// Copyright 2023 Open Source Robotics Foundation, Inc.
// Copyright 2016-2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <mutex>

#include "guard_condition.hpp"

#include "rmw_data_types.hpp"

///==============================================================================
GuardCondition::GuardCondition(rmw_context_impl_t * context_impl)
: context_impl_(context_impl)
{
}

///==============================================================================
void GuardCondition::trigger()
{
  std::lock_guard<std::mutex> lk(context_impl_->handles_mutex);
  context_impl_->handles.insert(this);
  context_impl_->handles_cv.notify_all();
}
