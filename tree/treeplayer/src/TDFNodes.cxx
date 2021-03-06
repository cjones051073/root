// Author: Enrico Guiraud, Danilo Piparo CERN  03/2017

/*************************************************************************
 * Copyright (C) 1995-2016, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "RConfigure.h" // R__USE_IMT
#include "ROOT/TCutFlowReport.hxx"
#include "ROOT/TDFNodes.hxx"
#include "ROOT/TSpinMutex.hxx"
#include "ROOT/TTreeProcessorMT.hxx"
#ifdef R__USE_IMT
#include "ROOT/TThreadExecutor.hxx"
#endif
#include "RtypesCore.h" // Long64_t
#include "TInterpreter.h"
#include "TROOT.h" // IsImplicitMTEnabled
#include "TTreeReader.h"

#include <cassert>
#include <mutex>
#include <string>
class TDirectory;
class TTree;
using namespace ROOT::Detail::TDF;
using namespace ROOT::Internal::TDF;

namespace ROOT {
namespace Internal {
namespace TDF {

TActionBase::TActionBase(TLoopManager *implPtr, const unsigned int nSlots) : fImplPtr(implPtr), fNSlots(nSlots)
{
}

} // end NS TDF
} // end NS Internal
} // end NS ROOT

TCustomColumnBase::TCustomColumnBase(TLoopManager *implPtr, std::string_view name, const unsigned int nSlots,
                                     const bool isDSColumn)
   : fImplPtr(implPtr), fName(name), fNSlots(nSlots), fIsDataSourceColumn(isDSColumn)
{
}

std::string TCustomColumnBase::GetName() const
{
   return fName;
}

TLoopManager *TCustomColumnBase::GetImplPtr() const
{
   return fImplPtr;
}

void TCustomColumnBase::InitNode()
{
   fLastCheckedEntry = std::vector<Long64_t>(fNSlots, -1);
}

TFilterBase::TFilterBase(TLoopManager *implPtr, std::string_view name, const unsigned int nSlots)
   : fImplPtr(implPtr), fLastResult(nSlots), fAccepted(nSlots), fRejected(nSlots), fName(name), fNSlots(nSlots)
{
}

TLoopManager *TFilterBase::GetImplPtr() const
{
   return fImplPtr;
}

bool TFilterBase::HasName() const
{
   return !fName.empty();
};

void TFilterBase::FillReport(ROOT::Experimental::TDF::TCutFlowReport &rep) const
{
   if (fName.empty()) // FillReport is no-op for unnamed filters
      return;
   const auto accepted = std::accumulate(fAccepted.begin(), fAccepted.end(), 0ULL);
   const auto all = accepted + std::accumulate(fRejected.begin(), fRejected.end(), 0ULL);
   rep.AddCut({fName, accepted, all});
}

void TFilterBase::InitNode()
{
   fLastCheckedEntry = std::vector<Long64_t>(fNSlots, -1);
   if (!fName.empty()) // if this is a named filter we care about its report count
      ResetReportCount();
}

void TSlotStack::ReturnSlot(unsigned int slotNumber)
{
   auto &index = GetIndex();
   auto &count = GetCount();
   assert(count > 0U && "TSlotStack has a reference count relative to an index which will become negative.");
   count--;
   if (0U == count) {
      index = UINT_MAX;
      std::lock_guard<ROOT::TSpinMutex> guard(fMutex);
      fBuf[fCursor++] = slotNumber;
      assert(fCursor <= fBuf.size() && "TSlotStack assumes that at most a fixed number of values can be present in the "
                                       "stack. fCursor is greater than the size of the internal buffer. This violates "
                                       "such assumption.");
   }
}

unsigned int TSlotStack::GetSlot()
{
   auto &index = GetIndex();
   auto &count = GetCount();
   count++;
   if (UINT_MAX != index)
      return index;
   std::lock_guard<ROOT::TSpinMutex> guard(fMutex);
   assert(fCursor > 0 && "TSlotStack assumes that a value can be always obtained. In this case fCursor is <=0 and this "
                         "violates such assumption.");
   index = fBuf[--fCursor];
   return index;
}

TLoopManager::TLoopManager(TTree *tree, const ColumnNames_t &defaultBranches)
   : fTree(std::shared_ptr<TTree>(tree, [](TTree *) {})), fDefaultColumns(defaultBranches),
     fNSlots(TDFInternal::GetNSlots()),
     fLoopType(ROOT::IsImplicitMTEnabled() ? ELoopType::kROOTFilesMT : ELoopType::kROOTFiles)
{
}

TLoopManager::TLoopManager(ULong64_t nEmptyEntries)
   : fNEmptyEntries(nEmptyEntries), fNSlots(TDFInternal::GetNSlots()),
     fLoopType(ROOT::IsImplicitMTEnabled() ? ELoopType::kNoFilesMT : ELoopType::kNoFiles)
{
}

TLoopManager::TLoopManager(std::unique_ptr<TDataSource> ds, const ColumnNames_t &defaultBranches)
   : fDefaultColumns(defaultBranches), fNSlots(TDFInternal::GetNSlots()),
     fLoopType(ROOT::IsImplicitMTEnabled() ? ELoopType::kDataSourceMT : ELoopType::kDataSource),
     fDataSource(std::move(ds))
{
   fDataSource->SetNSlots(fNSlots);
}

/// Run event loop with no source files, in parallel.
void TLoopManager::RunEmptySourceMT()
{
#ifdef R__USE_IMT
   TSlotStack slotStack(fNSlots);
   // Working with an empty tree.
   // Evenly partition the entries according to fNSlots. Produce around 2 tasks per slot.
   const auto nEntriesPerSlot = fNEmptyEntries / (fNSlots * 2);
   auto remainder = fNEmptyEntries % (fNSlots * 2);
   std::vector<std::pair<ULong64_t, ULong64_t>> entryRanges;
   ULong64_t start = 0;
   while (start < fNEmptyEntries) {
      ULong64_t end = start + nEntriesPerSlot;
      if (remainder > 0) {
         ++end;
         --remainder;
      }
      entryRanges.emplace_back(start, end);
      start = end;
   }

   // Each task will generate a subrange of entries
   auto genFunction = [this, &slotStack](const std::pair<ULong64_t, ULong64_t> &range) {
      auto slot = slotStack.GetSlot();
      InitNodeSlots(nullptr, slot);
      for (auto currEntry = range.first; currEntry < range.second; ++currEntry) {
         RunAndCheckFilters(slot, currEntry);
      }
      CleanUpTask(slot);
      slotStack.ReturnSlot(slot);
   };

   ROOT::TThreadExecutor pool;
   pool.Foreach(genFunction, entryRanges);

#endif // not implemented otherwise
}

/// Run event loop with no source files, in sequence.
void TLoopManager::RunEmptySource()
{
   InitNodeSlots(nullptr, 0);
   for (ULong64_t currEntry = 0; currEntry < fNEmptyEntries && fNStopsReceived < fNChildren; ++currEntry) {
      RunAndCheckFilters(0, currEntry);
   }
}

/// Run event loop over one or multiple ROOT files, in parallel.
void TLoopManager::RunTreeProcessorMT()
{
#ifdef R__USE_IMT
   TSlotStack slotStack(fNSlots);
   using ttpmt_t = ROOT::TTreeProcessorMT;
   std::unique_ptr<ttpmt_t> tp;
   tp.reset(new ttpmt_t(*fTree));

   tp->Process([this, &slotStack](TTreeReader &r) -> void {
      auto slot = slotStack.GetSlot();
      InitNodeSlots(&r, slot);
      // recursive call to check filters and conditionally execute actions
      while (r.Next()) {
         RunAndCheckFilters(slot, r.GetCurrentEntry());
      }
      CleanUpTask(slot);
      slotStack.ReturnSlot(slot);
   });
#endif // no-op otherwise (will not be called)
}

/// Run event loop over one or multiple ROOT files, in sequence.
void TLoopManager::RunTreeReader()
{
   TTreeReader r(fTree.get());
   if (0 == fTree->GetEntriesFast())
      return;
   InitNodeSlots(&r, 0);

   // recursive call to check filters and conditionally execute actions
   // in the non-MT case processing can be stopped early by ranges, hence the check on fNStopsReceived
   while (r.Next() && fNStopsReceived < fNChildren) {
      RunAndCheckFilters(0, r.GetCurrentEntry());
   }
   fTree->GetEntry(0);
}

/// Run event loop over data accessed through a DataSource, in sequence.
void TLoopManager::RunDataSource()
{
   assert(fDataSource != nullptr);
   fDataSource->Initialise();
   auto ranges = fDataSource->GetEntryRanges();
   while (!ranges.empty()) {
      InitNodeSlots(nullptr, 0u);
      fDataSource->InitSlot(0u, 0ull);
      for (const auto &range : ranges) {
         auto end = range.second;
         for (auto entry = range.first; entry < end; ++entry) {
            fDataSource->SetEntry(0u, entry);
            RunAndCheckFilters(0u, entry);
         }
      }
      fDataSource->FinaliseSlot(0u);
      ranges = fDataSource->GetEntryRanges();
   }
   fDataSource->Finalise();
}

/// Run event loop over data accessed through a DataSource, in parallel.
void TLoopManager::RunDataSourceMT()
{
#ifdef R__USE_IMT
   assert(fDataSource != nullptr);
   TSlotStack slotStack(fNSlots);
   ROOT::TThreadExecutor pool;

   // Each task works on a subrange of entries
   auto runOnRange = [this, &slotStack](const std::pair<ULong64_t, ULong64_t> &range) {
      const auto slot = slotStack.GetSlot();
      InitNodeSlots(nullptr, slot);
      fDataSource->InitSlot(slot, range.first);
      const auto end = range.second;
      for (auto entry = range.first; entry < end; ++entry) {
         fDataSource->SetEntry(slot, entry);
         RunAndCheckFilters(slot, entry);
      }
      CleanUpTask(slot);
      fDataSource->FinaliseSlot(slot);
      slotStack.ReturnSlot(slot);
   };

   fDataSource->Initialise();
   auto ranges = fDataSource->GetEntryRanges();
   while (!ranges.empty()) {
      pool.Foreach(runOnRange, ranges);
      ranges = fDataSource->GetEntryRanges();
   }
   fDataSource->Finalise();
#endif // not implemented otherwise (never called)
}

/// Execute actions and make sure named filters are called for each event.
/// Named filters must be called even if the analysis logic would not require it, lest they report confusing results.
void TLoopManager::RunAndCheckFilters(unsigned int slot, Long64_t entry)
{
   for (auto &actionPtr : fBookedActions)
      actionPtr->Run(slot, entry);
   for (auto &namedFilterPtr : fBookedNamedFilters)
      namedFilterPtr->CheckFilters(slot, entry);
   for (auto &callback : fCallbacks)
      callback(slot);
}

/// Build TTreeReaderValues for all nodes
/// This method loops over all filters, actions and other booked objects and
/// calls their `InitTDFValues` methods. It is called once per node per slot, before
/// running the event loop. It also informs each node of the TTreeReader that
/// a particular slot will be using.
void TLoopManager::InitNodeSlots(TTreeReader *r, unsigned int slot)
{
   // booked branches must be initialized first because other nodes might need to point to the values they encapsulate
   for (auto &bookedBranch : fBookedCustomColumns)
      bookedBranch.second->InitSlot(r, slot);
   for (auto &ptr : fBookedActions)
      ptr->InitSlot(r, slot);
   for (auto &ptr : fBookedFilters)
      ptr->InitSlot(r, slot);
   for (auto &callback : fCallbacksOnce)
      callback(slot);
}

/// Initialize all nodes of the functional graph before running the event loop.
/// This method is called once per event-loop and performs generic initialization
/// operations that do not depend on the specific processing slot (i.e. operations
/// that are common for all threads).
void TLoopManager::InitNodes()
{
   EvalChildrenCounts();
   for (auto &filter : fBookedFilters)
      filter->InitNode();
   for (auto &customColumn : fBookedCustomColumns)
      customColumn.second->InitNode();
}

/// Perform clean-up operations. To be called at the end of each event loop.
void TLoopManager::CleanUpNodes()
{
   fMustRunNamedFilters = false;

   // forget TActions and detach TResultProxies
   fBookedActions.clear();
   for (auto readiness : fResProxyReadiness) {
      *readiness.get() = true;
   }
   fResProxyReadiness.clear();

   // reset children counts
   fNChildren = 0;
   fNStopsReceived = 0;
   for (auto &ptr : fBookedFilters)
      ptr->ResetChildrenCount();
   for (auto &ptr : fBookedRanges)
      ptr->ResetChildrenCount();

   fCallbacks.clear();
   fCallbacksOnce.clear();
}

/// Perform clean-up operations. To be called at the end of each task execution.
void TLoopManager::CleanUpTask(unsigned int slot)
{
   for (auto &ptr : fBookedActions)
      ptr->ClearValueReaders(slot);
   for (auto &ptr : fBookedFilters)
      ptr->ClearValueReaders(slot);
   for (auto &pair : fBookedCustomColumns)
      pair.second->ClearValueReaders(slot);
}

/// Jit all actions that required runtime column type inference, and clean the `fToJit` member variable.
void TLoopManager::JitActions()
{
   auto error = TInterpreter::EErrorCode::kNoError;
   gInterpreter->Calc(fToJit.c_str(), &error);
   if (TInterpreter::EErrorCode::kNoError != error) {
      std::string exceptionText =
         "An error occurred while jitting. The lines above might indicate the cause of the crash\n";
      throw std::runtime_error(exceptionText.c_str());
   }
   fToJit.clear();
}

/// Trigger counting of number of children nodes for each node of the functional graph.
/// This is done once before starting the event loop. Each action sends an `increase children count` signal
/// upstream, which is propagated until TLoopManager. Each time a node receives the signal, in increments its
/// children counter. Each node only propagates the signal once, even if it receives it multiple times.
/// Named filters also send an `increase children count` signal, just like actions, as they always execute during
/// the event loop so the graph branch they belong to must count as active even if it does not end in an action.
void TLoopManager::EvalChildrenCounts()
{
   for (auto &actionPtr : fBookedActions)
      actionPtr->TriggerChildrenCount();
   for (auto &namedFilterPtr : fBookedNamedFilters)
      namedFilterPtr->TriggerChildrenCount();
}

/// Start the event loop with a different mechanism depending on IMT/no IMT, data source/no data source.
/// Also perform a few setup and clean-up operations (jit actions if necessary, clear booked actions after the loop...).
void TLoopManager::Run()
{
   if (!fToJit.empty())
      JitActions();

   InitNodes();

   switch (fLoopType) {
   case ELoopType::kNoFilesMT: RunEmptySourceMT(); break;
   case ELoopType::kROOTFilesMT: RunTreeProcessorMT(); break;
   case ELoopType::kDataSourceMT: RunDataSourceMT(); break;
   case ELoopType::kNoFiles: RunEmptySource(); break;
   case ELoopType::kROOTFiles: RunTreeReader(); break;
   case ELoopType::kDataSource: RunDataSource(); break;
   }

   CleanUpNodes();
}

TLoopManager *TLoopManager::GetImplPtr()
{
   return this;
}

/// Return the list of default columns -- empty if none was provided when constructing the TDataFrame
const ColumnNames_t &TLoopManager::GetDefaultColumnNames() const
{
   return fDefaultColumns;
}

TTree *TLoopManager::GetTree() const
{
   return fTree.get();
}

TCustomColumnBase *TLoopManager::GetBookedBranch(const std::string &name) const
{
   auto it = fBookedCustomColumns.find(name);
   return it == fBookedCustomColumns.end() ? nullptr : it->second.get();
}

TDirectory *TLoopManager::GetDirectory() const
{
   return fDirPtr;
}

void TLoopManager::Book(const ActionBasePtr_t &actionPtr)
{
   fBookedActions.emplace_back(actionPtr);
}

void TLoopManager::Book(const FilterBasePtr_t &filterPtr)
{
   fBookedFilters.emplace_back(filterPtr);
   if (filterPtr->HasName()) {
      fBookedNamedFilters.emplace_back(filterPtr);
      fMustRunNamedFilters = true;
   }
}

void TLoopManager::Book(const TCustomColumnBasePtr_t &columnPtr)
{
   const auto &name = columnPtr->GetName();
   fBookedCustomColumns[name] = columnPtr;
   fCustomColumnNames.emplace_back(name);
}

void TLoopManager::Book(const std::shared_ptr<bool> &readinessPtr)
{
   fResProxyReadiness.emplace_back(readinessPtr);
}

void TLoopManager::Book(const RangeBasePtr_t &rangePtr)
{
   fBookedRanges.emplace_back(rangePtr);
}

// dummy call, end of recursive chain of calls
bool TLoopManager::CheckFilters(int, unsigned int)
{
   return true;
}

/// Call `FillReport` on all booked filters
void TLoopManager::Report(ROOT::Experimental::TDF::TCutFlowReport &rep) const
{
   for (const auto &fPtr : fBookedNamedFilters)
      fPtr->FillReport(rep);
}

void TLoopManager::RegisterCallback(ULong64_t everyNEvents, std::function<void(unsigned int)> &&f)
{
   if (everyNEvents == 0ull)
      fCallbacksOnce.emplace_back(std::move(f), fNSlots);
   else
      fCallbacks.emplace_back(everyNEvents, std::move(f), fNSlots);
}

TRangeBase::TRangeBase(TLoopManager *implPtr, unsigned int start, unsigned int stop, unsigned int stride,
                       const unsigned int nSlots)
   : fImplPtr(implPtr), fStart(start), fStop(stop), fStride(stride), fNSlots(nSlots)
{
}

TLoopManager *TRangeBase::GetImplPtr() const
{
   return fImplPtr;
}
