/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/python/py_executable.h"

#include "absl/algorithm/container.h"
#include "tensorflow/core/platform/fingerprint.h"

namespace xla {

namespace py = pybind11;

PyExecutable::PyExecutable(std::shared_ptr<PyClient> client,
                           std::unique_ptr<PjRtExecutable> executable,
                           std::shared_ptr<Traceback> traceback,
                           absl::optional<std::string> fingerprint)
    : client_(std::move(client)),
      executable_(std::move(executable)),
      traceback_(std::move(traceback)),
      fingerprint_(std::move(fingerprint)) {
  CHECK(PyGILState_Check());
  next_ = client_->executables_;
  client_->executables_ = this;
  prev_ = nullptr;
  if (next_) {
    next_->prev_ = this;
  }
  options_.untuple_result = true;
  if (fingerprint_) {
    options_.launch_id = tensorflow::Fingerprint32(*fingerprint_);
    VLOG(1) << "Fingerprint for executable " << executable_->name() << ": "
            << *fingerprint_;
  }
}

PyExecutable::~PyExecutable() {
  CHECK(PyGILState_Check());
  if (client_->executables_ == this) {
    client_->executables_ = next_;
  }
  if (prev_) {
    prev_->next_ = next_;
  }
  if (next_) {
    next_->prev_ = prev_;
  }
}

std::vector<ClientAndPtr<PjRtDevice>> PyExecutable::LocalDevices() const {
  std::vector<ClientAndPtr<PjRtDevice>> devices;
  devices.reserve(executable_->local_devices().size());
  for (PjRtDevice* device : executable_->local_devices()) {
    devices.push_back(WrapWithClient(client_, device));
  }
  return devices;
}

StatusOr<std::vector<std::unique_ptr<PyBuffer>>> PyExecutable::PjRtExecute(
    absl::Span<PjRtBuffer* const> args) {
  std::vector<std::unique_ptr<PjRtBuffer>> output_buffers;
  {
    py::gil_scoped_release gil_release;
    TF_ASSIGN_OR_RETURN(output_buffers, executable_->Execute(args, options_));
  }
  auto traceback = Traceback::Get();
  std::vector<std::unique_ptr<PyBuffer>> outputs;
  outputs.reserve(output_buffers.size());
  for (auto& buffer : output_buffers) {
    outputs.push_back(
        std::make_unique<PyBuffer>(client_, std::move(buffer), traceback));
  }
  return outputs;
}

StatusOr<std::vector<std::unique_ptr<PyBuffer>>> PyExecutable::Execute(
    absl::Span<PyBuffer* const> args) {
  std::vector<std::unique_ptr<PjRtBuffer>> output_buffers;
  {
    py::gil_scoped_release gil_release;
    std::vector<PjRtBuffer*> arg_buffers(args.size());
    absl::c_transform(args, arg_buffers.begin(),
                      [](PyBuffer* buf) { return buf->buffer(); });
    TF_ASSIGN_OR_RETURN(output_buffers,
                        executable_->Execute(arg_buffers, options_));
  }
  auto traceback = Traceback::Get();
  std::vector<std::unique_ptr<PyBuffer>> outputs;
  outputs.reserve(output_buffers.size());
  for (auto& buffer : output_buffers) {
    outputs.push_back(
        std::make_unique<PyBuffer>(client_, std::move(buffer), traceback));
  }
  return outputs;
}

StatusOr<std::vector<std::vector<std::unique_ptr<PyBuffer>>>>
PyExecutable::ExecuteOnLocalDevices(
    absl::Span<const std::vector<PyBuffer*>> args) {
  std::vector<std::vector<std::unique_ptr<PjRtBuffer>>> output_buffers;
  {
    py::gil_scoped_release gil_release;
    std::vector<std::vector<PjRtBuffer*>> arg_buffers(args.size());
    for (int computation = 0; computation < args.size(); ++computation) {
      arg_buffers[computation].resize(args[computation].size());
      absl::c_transform(args[computation], arg_buffers[computation].begin(),
                        [](PyBuffer* buf) { return buf->buffer(); });
    }
    TF_ASSIGN_OR_RETURN(output_buffers, executable_->ExecuteOnLocalDevices(
                                            arg_buffers, options_));
  }
  auto traceback = Traceback::Get();
  std::vector<std::vector<std::unique_ptr<PyBuffer>>> outputs;
  outputs.resize(output_buffers.size());
  for (int computation = 0; computation < output_buffers.size();
       ++computation) {
    for (auto& buffer : output_buffers[computation]) {
      outputs[computation].push_back(
          std::make_unique<PyBuffer>(client_, std::move(buffer), traceback));
    }
  }
  return outputs;
}

StatusOr<std::vector<std::shared_ptr<HloModule>>> PyExecutable::HloModules()
    const {
  return executable_->GetHloModules();
}

}  // namespace xla
