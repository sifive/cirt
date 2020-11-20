//===- State.cpp - LLHD simulator state -------------------------*- C++ -*-===//
//
// This file implements the constructs used to keep track of the simulation
// state in the LLHD simulator.
//
//===----------------------------------------------------------------------===//

#include "State.h"

#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace llvm;
using namespace circt::llhd::sim;

//===----------------------------------------------------------------------===//
// Time
//===----------------------------------------------------------------------===//

bool Time::operator<(const Time &rhs) const {
  if (time < rhs.time)
    return true;
  if (time == rhs.time && delta < rhs.delta)
    return true;
  if (time == rhs.time && delta == rhs.delta && eps < rhs.eps)
    return true;
  return false;
}

bool Time::operator==(const Time &rhs) const {
  return (time == rhs.time && delta == rhs.delta && eps == rhs.eps);
}

Time Time::operator+(const Time &rhs) const {
  return Time(time + rhs.time, delta + rhs.delta, eps + rhs.eps);
}

bool Time::isZero() { return (time == 0 && delta == 0 && eps == 0); }

std::string Time::dump() {
  std::string ret;
  raw_string_ostream ss(ret);
  ss << time << "ps " << delta << "d " << eps << "e";
  return ss.str();
}

//===----------------------------------------------------------------------===//
// Signal
//===----------------------------------------------------------------------===//

Signal::Signal(std::string name, std::string owner)
    : name(name), owner(owner), size(0), value(nullptr) {}

Signal::Signal(std::string name, std::string owner, uint8_t *value,
               uint64_t size)
    : name(name), owner(owner), size(size), value(value) {}

bool Signal::operator==(const Signal &rhs) const {
  if (owner != rhs.owner || name != rhs.name || size != rhs.size)
    return false;
  return std::memcmp(value.get(), rhs.value.get(), size);
}

bool Signal::operator<(const Signal &rhs) const {
  if (owner < rhs.owner)
    return true;
  if (owner == rhs.owner && name < rhs.name)
    return true;
  return false;
}

std::string Signal::dump() {
  std::string ret;
  raw_string_ostream ss(ret);
  ss << "0x";
  for (int i = size - 1; i >= 0; --i) {
    ss << format_hex_no_prefix(static_cast<int>(value.get()[i]), 2);
  }
  return ss.str();
}

std::string Signal::dump(unsigned elemIndex) {
  assert(elements.size() > 0 && "the signal type has to be tuple or array!");
  auto elemSize = elements[elemIndex].second;
  auto ptr = value.get() + elements[elemIndex].first;
  std::string ret;
  raw_string_ostream ss(ret);
  ss << "0x";
  for (int i = elemSize - 1; i >= 0; --i) {
    ss << format_hex_no_prefix(static_cast<int>(ptr[i]), 2);
  }
  return ret;
}
//===----------------------------------------------------------------------===//
// Slot
//===----------------------------------------------------------------------===//

Slot::Slot(Time time, int index, int bitOffset, uint8_t *bytes, unsigned width)
    : time(time) {
  insertChange(index, bitOffset, bytes, width);
}

bool Slot::operator<(const Slot &rhs) const { return time < rhs.time; }

bool Slot::operator>(const Slot &rhs) const { return rhs.time < time; }

void Slot::insertChange(int index, int bitOffset, uint8_t *bytes,
                        unsigned width) {
  auto size = llvm::divideCeil(width, 64);

  if (changesSize >= buffers.size()) {
    // Create a new change buffer if we don't have any unused one available for
    // reuse.
    buffers.push_back(std::make_pair(
        bitOffset,
        APInt(width, makeArrayRef(reinterpret_cast<uint64_t *>(bytes), size))));
  } else {
    // Reuse the first available buffer.
    buffers[changesSize] = std::make_pair(
        bitOffset,
        APInt(width, makeArrayRef(reinterpret_cast<uint64_t *>(bytes), size)));
  }

  // Map the signal index to the change buffer so we can retrieve
  // it after sorting.
  changes.push_back(std::make_pair(index, changesSize));
  ++changesSize;
}

void Slot::insertChange(unsigned inst) { scheduled.push_back(inst); }

//===----------------------------------------------------------------------===//
// UpdateQueue
//===----------------------------------------------------------------------===//
void UpdateQueue::insertOrUpdate(Time time, int index, int bitOffset,
                                 uint8_t *bytes, unsigned width) {
  auto &s = begin()[topSlot];

  // Directly add to top slot.
  if (!s.unused && time == s.time) {
    s.insertChange(index, bitOffset, bytes, width);
    return;
  }

  // We need to search through the queue for an existing slot only if we're
  // spawning an event later than the current top slot. Adding to an existing
  // slot scheduled earlier than the top slot should never happen, as in that
  // case it should be the top.
  if (events > 0 && s.time < time) {
    for (size_t i = 0, e = size(); i < e; ++i) {
      if (time == begin()[i].time) {
        begin()[i].insertChange(index, bitOffset, bytes, width);
        return;
      }
    }
  }

  // Spawn new event and update top.
  if (unused.size() > 0) {
    // An unused slot available.
    auto u = unused.back();
    unused.pop_back();
    auto &curr = begin()[u];
    curr.insertChange(index, bitOffset, bytes, width);
    curr.unused = false;
    curr.time = time;
    if (s.unused || time < s.time)
      topSlot = u;
  } else {
    // We need to generate a new slot.
    push_back(Slot(time, index, bitOffset, bytes, width));
    if (s.unused || time < s.time)
      topSlot = size() - 1;
  }
  ++events;
}

void UpdateQueue::insertOrUpdate(Time time, unsigned inst) {
  auto &s = begin()[topSlot];

  // Directly add to top slot.
  if (!s.unused && time == s.time) {
    s.insertChange(inst);
    return;
  }

  // We need to search through the queue for an existing slot only if we're
  // spawning an event after the top slot. Adding to an existing slot scheduled
  // earlier than the top slot should never happens, as then it should be the
  // top.
  if (events > 0 && s.time < time) {
    // int firstUnused = -1;
    for (size_t i = 0, e = size(); i < e; ++i) {
      if (time == begin()[i].time) {
        begin()[i].insertChange(inst);
        return;
      }
    }
  }

  // Spawn new event.
  if (unused.size() > 0) {
    auto u = unused.back();
    unused.pop_back();
    auto &curr = begin()[u];
    curr.insertChange(inst);
    curr.unused = false;
    curr.time = time;
    if (s.unused || time < s.time)
      topSlot = u;
  } else {
    Slot slot(time);
    slot.insertChange(inst);
    push_back(slot);
    if (s.unused || time < s.time)
      topSlot = size() - 1;
  }
  ++events;
}

const Slot &UpdateQueue::top() {
  assert(topSlot < size() && "top is pointing out of bounds!");
  auto &top = begin()[topSlot];
  std::sort(top.changes.begin(), top.changes.begin() + top.changesSize);
  return top;
}

void UpdateQueue::pop() {
  auto &curr = begin()[topSlot];
  curr.unused = true;
  curr.changesSize = 0;
  curr.scheduled.clear();
  curr.changes.clear();
  curr.time = Time();
  --events;

  unused.push_back(topSlot);
  topSlot = std::distance(
      begin(), std::min_element(begin(), end(), [](auto &a, auto &b) {
        return !a.unused && (a < b || b.unused);
      }));
}

//===----------------------------------------------------------------------===//
// State
//===----------------------------------------------------------------------===//

State::~State() {
  for (auto &inst : instances) {
    if (inst.procState) {
      std::free(inst.procState->senses);
    }
  }
}

Slot State::popQueue() {
  assert(!queue.empty() && "the event queue is empty");
  Slot pop = queue.top();
  queue.pop();
  return pop;
}

void State::pushQueue(Time t, unsigned inst) {
  Time newTime = time + t;
  queue.insertOrUpdate(newTime, inst);
  instances[inst].expectedWakeup = newTime;
}

std::vector<Instance>::iterator
State::getInstanceIterator(std::string instName) {
  auto it =
      std::find_if(instances.begin(), instances.end(),
                   [&](const auto &inst) { return instName == inst.name; });
  if (it == instances.end()) {
    llvm::errs() << "could not find an instance named " << instName << "\n";
    exit(EXIT_FAILURE);
  }
  return it;
}

int State::addSignal(std::string name, std::string owner) {
  signals.push_back(Signal(name, owner));
  return signals.size() - 1;
}

void State::addProcPtr(std::string name, ProcState *procStatePtr) {
  auto it = getInstanceIterator(name);

  // Store instance index in process state.
  procStatePtr->inst = it - instances.begin();
  (*it).procState = std::unique_ptr<ProcState>(procStatePtr);
}

int State::addSignalData(int index, std::string owner, uint8_t *value,
                         uint64_t size) {
  auto it = getInstanceIterator(owner);

  uint64_t globalIdx = (*it).sensitivityList[index + (*it).nArgs].globalIndex;
  auto &sig = signals[globalIdx];

  // Add pointer and size to global signal table entry.
  sig.value = std::unique_ptr<uint8_t>(value);
  sig.size = size;

  // For non-integer signals, set their width equal to the size of the type in
  // memory.
  if (sig.width == 0)
    sig.width = size;

  // Add the value pointer to the signal detail struct for each instance this
  // signal appears in.
  for (auto inst : signals[globalIdx].triggers) {
    for (auto &detail : instances[inst].sensitivityList) {
      if (detail.globalIndex == globalIdx) {
        detail.value = sig.value.get();
      }
    }
  }
  return globalIdx;
}

void State::addSignalElement(unsigned index, unsigned offset, unsigned size) {
  signals[index].elements.push_back(std::make_pair(offset, size));
}

void State::dumpSignal(llvm::raw_ostream &out, int index) {
  auto &sig = signals[index];
  for (auto inst : sig.triggers) {
    out << time.dump() << "  " << instances[inst].path << "/" << sig.name
        << "  " << sig.dump() << "\n";
  }
}

void State::dumpLayout() {
  llvm::errs() << "::------------------- Layout -------------------::\n";
  for (const auto &inst : instances) {
    llvm::errs() << inst.name << ":\n";
    llvm::errs() << "---path: " << inst.path << "\n";
    llvm::errs() << "---isEntity: " << inst.isEntity << "\n";
    llvm::errs() << "---sensitivity list: ";
    for (auto in : inst.sensitivityList) {
      llvm::errs() << in.globalIndex << " ";
    }
    llvm::errs() << "\n";
  }
  llvm::errs() << "::----------------------------------------------::\n";
}

void State::dumpSignalTriggers() {
  llvm::errs() << "::------------- Signal information -------------::\n";
  for (size_t i = 0, e = signals.size(); i < e; ++i) {
    llvm::errs() << signals[i].owner << "/" << signals[i].name << " triggers: ";
    for (auto trig : signals[i].triggers) {
      llvm::errs() << trig << " ";
    }
    llvm::errs() << "\n";
  }
  llvm::errs() << "::----------------------------------------------::\n";
}
