/*
 * Copyright (C) 2022, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>
#include <string>

#include "aidl_language.h"
#include "aidl_to_cpp.h"
#include "aidl_to_cpp_common.h"
#include "options.h"

namespace android {
namespace aidl {
namespace cpp {

bool GenerateCppAnalyzer(const string& output_file, const Options& options,
                         const AidlTypenames& typenames, const AidlDefinedType& parsed_doc,
                         const IoDelegate& io_delegate);

}  // namespace cpp
}  // namespace aidl
}  // namespace android
