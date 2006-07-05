/*
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Kevin Lim
 *          Korey Sewell
 */

#include <limits>
#include <vector>

#include "sim/root.hh"

#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/inst_queue.hh"

using namespace std;

template <class Impl>
InstructionQueue<Impl>::FUCompletion::FUCompletion(DynInstPtr &_inst,
                                                   int fu_idx,
                                                   InstructionQueue<Impl> *iq_ptr)
    : Event(&mainEventQueue, Stat_Event_Pri),
      inst(_inst), fuIdx(fu_idx), iqPtr(iq_ptr), freeFU(false)
{
    this->setFlags(Event::AutoDelete);
}

template <class Impl>
void
InstructionQueue<Impl>::FUCompletion::process()
{
    iqPtr->processFUCompletion(inst, freeFU ? fuIdx : -1);
    inst = NULL;
}


template <class Impl>
const char *
InstructionQueue<Impl>::FUCompletion::description()
{
    return "Functional unit completion event";
}

template <class Impl>
InstructionQueue<Impl>::InstructionQueue(Params *params)
    : fuPool(params->fuPool),
      numEntries(params->numIQEntries),
      totalWidth(params->issueWidth),
      numPhysIntRegs(params->numPhysIntRegs),
      numPhysFloatRegs(params->numPhysFloatRegs),
      commitToIEWDelay(params->commitToIEWDelay)
{
    assert(fuPool);

    switchedOut = false;

    numThreads = params->numberOfThreads;

    // Set the number of physical registers as the number of int + float
    numPhysRegs = numPhysIntRegs + numPhysFloatRegs;

    DPRINTF(IQ, "There are %i physical registers.\n", numPhysRegs);

    //Create an entry for each physical register within the
    //dependency graph.
    dependGraph.resize(numPhysRegs);

    // Resize the register scoreboard.
    regScoreboard.resize(numPhysRegs);

    //Initialize Mem Dependence Units
    for (int i = 0; i < numThreads; i++) {
        memDepUnit[i].init(params,i);
        memDepUnit[i].setIQ(this);
    }

    resetState();

    string policy = params->smtIQPolicy;

    //Convert string to lowercase
    std::transform(policy.begin(), policy.end(), policy.begin(),
                   (int(*)(int)) tolower);

    //Figure out resource sharing policy
    if (policy == "dynamic") {
        iqPolicy = Dynamic;

        //Set Max Entries to Total ROB Capacity
        for (int i = 0; i < numThreads; i++) {
            maxEntries[i] = numEntries;
        }

    } else if (policy == "partitioned") {
        iqPolicy = Partitioned;

        //@todo:make work if part_amt doesnt divide evenly.
        int part_amt = numEntries / numThreads;

        //Divide ROB up evenly
        for (int i = 0; i < numThreads; i++) {
            maxEntries[i] = part_amt;
        }

        DPRINTF(IQ, "IQ sharing policy set to Partitioned:"
                "%i entries per thread.\n",part_amt);

    } else if (policy == "threshold") {
        iqPolicy = Threshold;

        double threshold =  (double)params->smtIQThreshold / 100;

        int thresholdIQ = (int)((double)threshold * numEntries);

        //Divide up by threshold amount
        for (int i = 0; i < numThreads; i++) {
            maxEntries[i] = thresholdIQ;
        }

        DPRINTF(IQ, "IQ sharing policy set to Threshold:"
                "%i entries per thread.\n",thresholdIQ);
   } else {
       assert(0 && "Invalid IQ Sharing Policy.Options Are:{Dynamic,"
              "Partitioned, Threshold}");
   }
}

template <class Impl>
InstructionQueue<Impl>::~InstructionQueue()
{
    dependGraph.reset();
#ifdef DEBUG
    cprintf("Nodes traversed: %i, removed: %i\n",
            dependGraph.nodesTraversed, dependGraph.nodesRemoved);
#endif
}

template <class Impl>
std::string
InstructionQueue<Impl>::name() const
{
    return cpu->name() + ".iq";
}

template <class Impl>
void
InstructionQueue<Impl>::regStats()
{
    using namespace Stats;
    iqInstsAdded
        .name(name() + ".iqInstsAdded")
        .desc("Number of instructions added to the IQ (excludes non-spec)")
        .prereq(iqInstsAdded);

    iqNonSpecInstsAdded
        .name(name() + ".iqNonSpecInstsAdded")
        .desc("Number of non-speculative instructions added to the IQ")
        .prereq(iqNonSpecInstsAdded);

    iqInstsIssued
        .name(name() + ".iqInstsIssued")
        .desc("Number of instructions issued")
        .prereq(iqInstsIssued);

    iqIntInstsIssued
        .name(name() + ".iqIntInstsIssued")
        .desc("Number of integer instructions issued")
        .prereq(iqIntInstsIssued);

    iqFloatInstsIssued
        .name(name() + ".iqFloatInstsIssued")
        .desc("Number of float instructions issued")
        .prereq(iqFloatInstsIssued);

    iqBranchInstsIssued
        .name(name() + ".iqBranchInstsIssued")
        .desc("Number of branch instructions issued")
        .prereq(iqBranchInstsIssued);

    iqMemInstsIssued
        .name(name() + ".iqMemInstsIssued")
        .desc("Number of memory instructions issued")
        .prereq(iqMemInstsIssued);

    iqMiscInstsIssued
        .name(name() + ".iqMiscInstsIssued")
        .desc("Number of miscellaneous instructions issued")
        .prereq(iqMiscInstsIssued);

    iqSquashedInstsIssued
        .name(name() + ".iqSquashedInstsIssued")
        .desc("Number of squashed instructions issued")
        .prereq(iqSquashedInstsIssued);

    iqSquashedInstsExamined
        .name(name() + ".iqSquashedInstsExamined")
        .desc("Number of squashed instructions iterated over during squash;"
              " mainly for profiling")
        .prereq(iqSquashedInstsExamined);

    iqSquashedOperandsExamined
        .name(name() + ".iqSquashedOperandsExamined")
        .desc("Number of squashed operands that are examined and possibly "
              "removed from graph")
        .prereq(iqSquashedOperandsExamined);

    iqSquashedNonSpecRemoved
        .name(name() + ".iqSquashedNonSpecRemoved")
        .desc("Number of squashed non-spec instructions that were removed")
        .prereq(iqSquashedNonSpecRemoved);

    queueResDist
        .init(Num_OpClasses, 0, 99, 2)
        .name(name() + ".IQ:residence:")
        .desc("cycles from dispatch to issue")
        .flags(total | pdf | cdf )
        ;
    for (int i = 0; i < Num_OpClasses; ++i) {
        queueResDist.subname(i, opClassStrings[i]);
    }
    numIssuedDist
        .init(0,totalWidth,1)
        .name(name() + ".ISSUE:issued_per_cycle")
        .desc("Number of insts issued each cycle")
        .flags(pdf)
        ;
/*
    dist_unissued
        .init(Num_OpClasses+2)
        .name(name() + ".ISSUE:unissued_cause")
        .desc("Reason ready instruction not issued")
        .flags(pdf | dist)
        ;
    for (int i=0; i < (Num_OpClasses + 2); ++i) {
        dist_unissued.subname(i, unissued_names[i]);
    }
*/
    statIssuedInstType
        .init(numThreads,Num_OpClasses)
        .name(name() + ".ISSUE:FU_type")
        .desc("Type of FU issued")
        .flags(total | pdf | dist)
        ;
    statIssuedInstType.ysubnames(opClassStrings);

    //
    //  How long did instructions for a particular FU type wait prior to issue
    //

    issueDelayDist
        .init(Num_OpClasses,0,99,2)
        .name(name() + ".ISSUE:")
        .desc("cycles from operands ready to issue")
        .flags(pdf | cdf)
        ;

    for (int i=0; i<Num_OpClasses; ++i) {
        stringstream subname;
        subname << opClassStrings[i] << "_delay";
        issueDelayDist.subname(i, subname.str());
    }

    issueRate
        .name(name() + ".ISSUE:rate")
        .desc("Inst issue rate")
        .flags(total)
        ;
    issueRate = iqInstsIssued / cpu->numCycles;

    statFuBusy
        .init(Num_OpClasses)
        .name(name() + ".ISSUE:fu_full")
        .desc("attempts to use FU when none available")
        .flags(pdf | dist)
        ;
    for (int i=0; i < Num_OpClasses; ++i) {
        statFuBusy.subname(i, opClassStrings[i]);
    }

    fuBusy
        .init(numThreads)
        .name(name() + ".ISSUE:fu_busy_cnt")
        .desc("FU busy when requested")
        .flags(total)
        ;

    fuBusyRate
        .name(name() + ".ISSUE:fu_busy_rate")
        .desc("FU busy rate (busy events/executed inst)")
        .flags(total)
        ;
    fuBusyRate = fuBusy / iqInstsIssued;

    for ( int i=0; i < numThreads; i++) {
        // Tell mem dependence unit to reg stats as well.
        memDepUnit[i].regStats();
    }
}

template <class Impl>
void
InstructionQueue<Impl>::resetState()
{
    //Initialize thread IQ counts
    for (int i = 0; i <numThreads; i++) {
        count[i] = 0;
        instList[i].clear();
    }

    // Initialize the number of free IQ entries.
    freeEntries = numEntries;

    // Note that in actuality, the registers corresponding to the logical
    // registers start off as ready.  However this doesn't matter for the
    // IQ as the instruction should have been correctly told if those
    // registers are ready in rename.  Thus it can all be initialized as
    // unready.
    for (int i = 0; i < numPhysRegs; ++i) {
        regScoreboard[i] = false;
    }

    for (int i = 0; i < numThreads; ++i) {
        squashedSeqNum[i] = 0;
    }

    for (int i = 0; i < Num_OpClasses; ++i) {
        while (!readyInsts[i].empty())
            readyInsts[i].pop();
        queueOnList[i] = false;
        readyIt[i] = listOrder.end();
    }
    nonSpecInsts.clear();
    listOrder.clear();
}

template <class Impl>
void
InstructionQueue<Impl>::setActiveThreads(list<unsigned> *at_ptr)
{
    DPRINTF(IQ, "Setting active threads list pointer.\n");
    activeThreads = at_ptr;
}

template <class Impl>
void
InstructionQueue<Impl>::setIssueToExecuteQueue(TimeBuffer<IssueStruct> *i2e_ptr)
{
    DPRINTF(IQ, "Set the issue to execute queue.\n");
    issueToExecuteQueue = i2e_ptr;
}

template <class Impl>
void
InstructionQueue<Impl>::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    DPRINTF(IQ, "Set the time buffer.\n");
    timeBuffer = tb_ptr;

    fromCommit = timeBuffer->getWire(-commitToIEWDelay);
}

template <class Impl>
void
InstructionQueue<Impl>::switchOut()
{
    resetState();
    dependGraph.reset();
    switchedOut = true;
    for (int i = 0; i < numThreads; ++i) {
        memDepUnit[i].switchOut();
    }
}

template <class Impl>
void
InstructionQueue<Impl>::takeOverFrom()
{
    switchedOut = false;
}

template <class Impl>
int
InstructionQueue<Impl>::entryAmount(int num_threads)
{
    if (iqPolicy == Partitioned) {
        return numEntries / num_threads;
    } else {
        return 0;
    }
}


template <class Impl>
void
InstructionQueue<Impl>::resetEntries()
{
    if (iqPolicy != Dynamic || numThreads > 1) {
        int active_threads = (*activeThreads).size();

        list<unsigned>::iterator threads  = (*activeThreads).begin();
        list<unsigned>::iterator list_end = (*activeThreads).end();

        while (threads != list_end) {
            if (iqPolicy == Partitioned) {
                maxEntries[*threads++] = numEntries / active_threads;
            } else if(iqPolicy == Threshold && active_threads == 1) {
                maxEntries[*threads++] = numEntries;
            }
        }
    }
}

template <class Impl>
unsigned
InstructionQueue<Impl>::numFreeEntries()
{
    return freeEntries;
}

template <class Impl>
unsigned
InstructionQueue<Impl>::numFreeEntries(unsigned tid)
{
    return maxEntries[tid] - count[tid];
}

// Might want to do something more complex if it knows how many instructions
// will be issued this cycle.
template <class Impl>
bool
InstructionQueue<Impl>::isFull()
{
    if (freeEntries == 0) {
        return(true);
    } else {
        return(false);
    }
}

template <class Impl>
bool
InstructionQueue<Impl>::isFull(unsigned tid)
{
    if (numFreeEntries(tid) == 0) {
        return(true);
    } else {
        return(false);
    }
}

template <class Impl>
bool
InstructionQueue<Impl>::hasReadyInsts()
{
    if (!listOrder.empty()) {
        return true;
    }

    for (int i = 0; i < Num_OpClasses; ++i) {
        if (!readyInsts[i].empty()) {
            return true;
        }
    }

    return false;
}

template <class Impl>
void
InstructionQueue<Impl>::insert(DynInstPtr &new_inst)
{
    // Make sure the instruction is valid
    assert(new_inst);

    DPRINTF(IQ, "Adding instruction [sn:%lli] PC %#x to the IQ.\n",
            new_inst->seqNum, new_inst->readPC());

    assert(freeEntries != 0);

    instList[new_inst->threadNumber].push_back(new_inst);

    --freeEntries;

    new_inst->setInIQ();

    // Look through its source registers (physical regs), and mark any
    // dependencies.
    addToDependents(new_inst);

    // Have this instruction set itself as the producer of its destination
    // register(s).
    addToProducers(new_inst);

    if (new_inst->isMemRef()) {
        memDepUnit[new_inst->threadNumber].insert(new_inst);
    } else {
        addIfReady(new_inst);
    }

    ++iqInstsAdded;

    count[new_inst->threadNumber]++;

    assert(freeEntries == (numEntries - countInsts()));
}

template <class Impl>
void
InstructionQueue<Impl>::insertNonSpec(DynInstPtr &new_inst)
{
    // @todo: Clean up this code; can do it by setting inst as unable
    // to issue, then calling normal insert on the inst.

    assert(new_inst);

    nonSpecInsts[new_inst->seqNum] = new_inst;

    DPRINTF(IQ, "Adding non-speculative instruction [sn:%lli] PC %#x "
            "to the IQ.\n",
            new_inst->seqNum, new_inst->readPC());

    assert(freeEntries != 0);

    instList[new_inst->threadNumber].push_back(new_inst);

    --freeEntries;

    new_inst->setInIQ();

    // Have this instruction set itself as the producer of its destination
    // register(s).
    addToProducers(new_inst);

    // If it's a memory instruction, add it to the memory dependency
    // unit.
    if (new_inst->isMemRef()) {
        memDepUnit[new_inst->threadNumber].insertNonSpec(new_inst);
    }

    ++iqNonSpecInstsAdded;

    count[new_inst->threadNumber]++;

    assert(freeEntries == (numEntries - countInsts()));
}

template <class Impl>
void
InstructionQueue<Impl>::insertBarrier(DynInstPtr &barr_inst)
{
    memDepUnit[barr_inst->threadNumber].insertBarrier(barr_inst);

    insertNonSpec(barr_inst);
}

template <class Impl>
typename Impl::DynInstPtr
InstructionQueue<Impl>::getInstToExecute()
{
    assert(!instsToExecute.empty());
    DynInstPtr inst = instsToExecute.front();
    instsToExecute.pop_front();
    return inst;
}

template <class Impl>
void
InstructionQueue<Impl>::addToOrderList(OpClass op_class)
{
    assert(!readyInsts[op_class].empty());

    ListOrderEntry queue_entry;

    queue_entry.queueType = op_class;

    queue_entry.oldestInst = readyInsts[op_class].top()->seqNum;

    ListOrderIt list_it = listOrder.begin();
    ListOrderIt list_end_it = listOrder.end();

    while (list_it != list_end_it) {
        if ((*list_it).oldestInst > queue_entry.oldestInst) {
            break;
        }

        list_it++;
    }

    readyIt[op_class] = listOrder.insert(list_it, queue_entry);
    queueOnList[op_class] = true;
}

template <class Impl>
void
InstructionQueue<Impl>::moveToYoungerInst(ListOrderIt list_order_it)
{
    // Get iterator of next item on the list
    // Delete the original iterator
    // Determine if the next item is either the end of the list or younger
    // than the new instruction.  If so, then add in a new iterator right here.
    // If not, then move along.
    ListOrderEntry queue_entry;
    OpClass op_class = (*list_order_it).queueType;
    ListOrderIt next_it = list_order_it;

    ++next_it;

    queue_entry.queueType = op_class;
    queue_entry.oldestInst = readyInsts[op_class].top()->seqNum;

    while (next_it != listOrder.end() &&
           (*next_it).oldestInst < queue_entry.oldestInst) {
        ++next_it;
    }

    readyIt[op_class] = listOrder.insert(next_it, queue_entry);
}

template <class Impl>
void
InstructionQueue<Impl>::processFUCompletion(DynInstPtr &inst, int fu_idx)
{
    // The CPU could have been sleeping until this op completed (*extremely*
    // long latency op).  Wake it if it was.  This may be overkill.
    if (isSwitchedOut()) {
        return;
    }

    iewStage->wakeCPU();

    if (fu_idx > -1)
        fuPool->freeUnitNextCycle(fu_idx);

    // @todo: Ensure that these FU Completions happen at the beginning
    // of a cycle, otherwise they could add too many instructions to
    // the queue.
    issueToExecuteQueue->access(0)->size++;
    instsToExecute.push_back(inst);
}

// @todo: Figure out a better way to remove the squashed items from the
// lists.  Checking the top item of each list to see if it's squashed
// wastes time and forces jumps.
template <class Impl>
void
InstructionQueue<Impl>::scheduleReadyInsts()
{
    DPRINTF(IQ, "Attempting to schedule ready instructions from "
            "the IQ.\n");

    IssueStruct *i2e_info = issueToExecuteQueue->access(0);

    // Have iterator to head of the list
    // While I haven't exceeded bandwidth or reached the end of the list,
    // Try to get a FU that can do what this op needs.
    // If successful, change the oldestInst to the new top of the list, put
    // the queue in the proper place in the list.
    // Increment the iterator.
    // This will avoid trying to schedule a certain op class if there are no
    // FUs that handle it.
    ListOrderIt order_it = listOrder.begin();
    ListOrderIt order_end_it = listOrder.end();
    int total_issued = 0;

    while (total_issued < totalWidth &&
           order_it != order_end_it) {
        OpClass op_class = (*order_it).queueType;

        assert(!readyInsts[op_class].empty());

        DynInstPtr issuing_inst = readyInsts[op_class].top();

        assert(issuing_inst->seqNum == (*order_it).oldestInst);

        if (issuing_inst->isSquashed()) {
            readyInsts[op_class].pop();

            if (!readyInsts[op_class].empty()) {
                moveToYoungerInst(order_it);
            } else {
                readyIt[op_class] = listOrder.end();
                queueOnList[op_class] = false;
            }

            listOrder.erase(order_it++);

            ++iqSquashedInstsIssued;

            continue;
        }

        int idx = -2;
        int op_latency = 1;
        int tid = issuing_inst->threadNumber;

        if (op_class != No_OpClass) {
            idx = fuPool->getUnit(op_class);

            if (idx > -1) {
                op_latency = fuPool->getOpLatency(op_class);
            }
        }

        // If we have an instruction that doesn't require a FU, or a
        // valid FU, then schedule for execution.
        if (idx == -2 || idx != -1) {
            if (op_latency == 1) {
                i2e_info->size++;
                instsToExecute.push_back(issuing_inst);

                // Add the FU onto the list of FU's to be freed next
                // cycle if we used one.
                if (idx >= 0)
                    fuPool->freeUnitNextCycle(idx);
            } else {
                int issue_latency = fuPool->getIssueLatency(op_class);
                // Generate completion event for the FU
                FUCompletion *execution = new FUCompletion(issuing_inst,
                                                           idx, this);

                execution->schedule(curTick + cpu->cycles(issue_latency - 1));

                // @todo: Enforce that issue_latency == 1 or op_latency
                if (issue_latency > 1) {
                    // If FU isn't pipelined, then it must be freed
                    // upon the execution completing.
                    execution->setFreeFU();
                } else {
                    // Add the FU onto the list of FU's to be freed next cycle.
                    fuPool->freeUnitNextCycle(idx);
                }
            }

            DPRINTF(IQ, "Thread %i: Issuing instruction PC %#x "
                    "[sn:%lli]\n",
                    tid, issuing_inst->readPC(),
                    issuing_inst->seqNum);

            readyInsts[op_class].pop();

            if (!readyInsts[op_class].empty()) {
                moveToYoungerInst(order_it);
            } else {
                readyIt[op_class] = listOrder.end();
                queueOnList[op_class] = false;
            }

            issuing_inst->setIssued();
            ++total_issued;

            if (!issuing_inst->isMemRef()) {
                // Memory instructions can not be freed from the IQ until they
                // complete.
                ++freeEntries;
                count[tid]--;
                issuing_inst->clearInIQ();
            } else {
                memDepUnit[tid].issue(issuing_inst);
            }

            listOrder.erase(order_it++);
            statIssuedInstType[tid][op_class]++;
        } else {
            statFuBusy[op_class]++;
            fuBusy[tid]++;
            ++order_it;
        }
    }

    numIssuedDist.sample(total_issued);
    iqInstsIssued+= total_issued;

    // If we issued any instructions, tell the CPU we had activity.
    if (total_issued) {
        cpu->activityThisCycle();
    } else {
        DPRINTF(IQ, "Not able to schedule any instructions.\n");
    }
}

template <class Impl>
void
InstructionQueue<Impl>::scheduleNonSpec(const InstSeqNum &inst)
{
    DPRINTF(IQ, "Marking nonspeculative instruction [sn:%lli] as ready "
            "to execute.\n", inst);

    NonSpecMapIt inst_it = nonSpecInsts.find(inst);

    assert(inst_it != nonSpecInsts.end());

    unsigned tid = (*inst_it).second->threadNumber;

    (*inst_it).second->setCanIssue();

    if (!(*inst_it).second->isMemRef()) {
        addIfReady((*inst_it).second);
    } else {
        memDepUnit[tid].nonSpecInstReady((*inst_it).second);
    }

    (*inst_it).second = NULL;

    nonSpecInsts.erase(inst_it);
}

template <class Impl>
void
InstructionQueue<Impl>::commit(const InstSeqNum &inst, unsigned tid)
{
    DPRINTF(IQ, "[tid:%i]: Committing instructions older than [sn:%i]\n",
            tid,inst);

    ListIt iq_it = instList[tid].begin();

    while (iq_it != instList[tid].end() &&
           (*iq_it)->seqNum <= inst) {
        ++iq_it;
        instList[tid].pop_front();
    }

    assert(freeEntries == (numEntries - countInsts()));
}

template <class Impl>
int
InstructionQueue<Impl>::wakeDependents(DynInstPtr &completed_inst)
{
    int dependents = 0;

    DPRINTF(IQ, "Waking dependents of completed instruction.\n");

    assert(!completed_inst->isSquashed());

    // Tell the memory dependence unit to wake any dependents on this
    // instruction if it is a memory instruction.  Also complete the memory
    // instruction at this point since we know it executed without issues.
    // @todo: Might want to rename "completeMemInst" to something that
    // indicates that it won't need to be replayed, and call this
    // earlier.  Might not be a big deal.
    if (completed_inst->isMemRef()) {
        memDepUnit[completed_inst->threadNumber].wakeDependents(completed_inst);
        completeMemInst(completed_inst);
    } else if (completed_inst->isMemBarrier() ||
               completed_inst->isWriteBarrier()) {
        memDepUnit[completed_inst->threadNumber].completeBarrier(completed_inst);
    }

    for (int dest_reg_idx = 0;
         dest_reg_idx < completed_inst->numDestRegs();
         dest_reg_idx++)
    {
        PhysRegIndex dest_reg =
            completed_inst->renamedDestRegIdx(dest_reg_idx);

        // Special case of uniq or control registers.  They are not
        // handled by the IQ and thus have no dependency graph entry.
        // @todo Figure out a cleaner way to handle this.
        if (dest_reg >= numPhysRegs) {
            continue;
        }

        DPRINTF(IQ, "Waking any dependents on register %i.\n",
                (int) dest_reg);

        //Go through the dependency chain, marking the registers as
        //ready within the waiting instructions.
        DynInstPtr dep_inst = dependGraph.pop(dest_reg);

        while (dep_inst) {
            DPRINTF(IQ, "Waking up a dependent instruction, PC%#x.\n",
                    dep_inst->readPC());

            // Might want to give more information to the instruction
            // so that it knows which of its source registers is
            // ready.  However that would mean that the dependency
            // graph entries would need to hold the src_reg_idx.
            dep_inst->markSrcRegReady();

            addIfReady(dep_inst);

            dep_inst = dependGraph.pop(dest_reg);

            ++dependents;
        }

        // Reset the head node now that all of its dependents have
        // been woken up.
        assert(dependGraph.empty(dest_reg));
        dependGraph.clearInst(dest_reg);

        // Mark the scoreboard as having that register ready.
        regScoreboard[dest_reg] = true;
    }
    return dependents;
}

template <class Impl>
void
InstructionQueue<Impl>::addReadyMemInst(DynInstPtr &ready_inst)
{
    OpClass op_class = ready_inst->opClass();

    readyInsts[op_class].push(ready_inst);

    // Will need to reorder the list if either a queue is not on the list,
    // or it has an older instruction than last time.
    if (!queueOnList[op_class]) {
        addToOrderList(op_class);
    } else if (readyInsts[op_class].top()->seqNum  <
               (*readyIt[op_class]).oldestInst) {
        listOrder.erase(readyIt[op_class]);
        addToOrderList(op_class);
    }

    DPRINTF(IQ, "Instruction is ready to issue, putting it onto "
            "the ready list, PC %#x opclass:%i [sn:%lli].\n",
            ready_inst->readPC(), op_class, ready_inst->seqNum);
}

template <class Impl>
void
InstructionQueue<Impl>::rescheduleMemInst(DynInstPtr &resched_inst)
{
    memDepUnit[resched_inst->threadNumber].reschedule(resched_inst);
}

template <class Impl>
void
InstructionQueue<Impl>::replayMemInst(DynInstPtr &replay_inst)
{
    memDepUnit[replay_inst->threadNumber].replay(replay_inst);
}

template <class Impl>
void
InstructionQueue<Impl>::completeMemInst(DynInstPtr &completed_inst)
{
    int tid = completed_inst->threadNumber;

    DPRINTF(IQ, "Completing mem instruction PC:%#x [sn:%lli]\n",
            completed_inst->readPC(), completed_inst->seqNum);

    ++freeEntries;

    completed_inst->memOpDone = true;

    memDepUnit[tid].completed(completed_inst);

    count[tid]--;
}

template <class Impl>
void
InstructionQueue<Impl>::violation(DynInstPtr &store,
                                  DynInstPtr &faulting_load)
{
    memDepUnit[store->threadNumber].violation(store, faulting_load);
}

template <class Impl>
void
InstructionQueue<Impl>::squash(unsigned tid)
{
    DPRINTF(IQ, "[tid:%i]: Starting to squash instructions in "
            "the IQ.\n", tid);

    // Read instruction sequence number of last instruction out of the
    // time buffer.
    squashedSeqNum[tid] = fromCommit->commitInfo[tid].doneSeqNum;

    // Call doSquash if there are insts in the IQ
    if (count[tid] > 0) {
        doSquash(tid);
    }

    // Also tell the memory dependence unit to squash.
    memDepUnit[tid].squash(squashedSeqNum[tid], tid);
}

template <class Impl>
void
InstructionQueue<Impl>::doSquash(unsigned tid)
{
    // Start at the tail.
    ListIt squash_it = instList[tid].end();
    --squash_it;

    DPRINTF(IQ, "[tid:%i]: Squashing until sequence number %i!\n",
            tid, squashedSeqNum[tid]);

    // Squash any instructions younger than the squashed sequence number
    // given.
    while (squash_it != instList[tid].end() &&
           (*squash_it)->seqNum > squashedSeqNum[tid]) {

        DynInstPtr squashed_inst = (*squash_it);

        // Only handle the instruction if it actually is in the IQ and
        // hasn't already been squashed in the IQ.
        if (squashed_inst->threadNumber != tid ||
            squashed_inst->isSquashedInIQ()) {
            --squash_it;
            continue;
        }

        if (!squashed_inst->isIssued() ||
            (squashed_inst->isMemRef() &&
             !squashed_inst->memOpDone)) {

            // Remove the instruction from the dependency list.
            if (!squashed_inst->isNonSpeculative() &&
                !squashed_inst->isStoreConditional() &&
                !squashed_inst->isMemBarrier() &&
                !squashed_inst->isWriteBarrier()) {

                for (int src_reg_idx = 0;
                     src_reg_idx < squashed_inst->numSrcRegs();
                     src_reg_idx++)
                {
                    PhysRegIndex src_reg =
                        squashed_inst->renamedSrcRegIdx(src_reg_idx);

                    // Only remove it from the dependency graph if it
                    // was placed there in the first place.

                    // Instead of doing a linked list traversal, we
                    // can just remove these squashed instructions
                    // either at issue time, or when the register is
                    // overwritten.  The only downside to this is it
                    // leaves more room for error.

                    if (!squashed_inst->isReadySrcRegIdx(src_reg_idx) &&
                        src_reg < numPhysRegs) {
                        dependGraph.remove(src_reg, squashed_inst);
                    }


                    ++iqSquashedOperandsExamined;
                }
            } else {
                NonSpecMapIt ns_inst_it =
                    nonSpecInsts.find(squashed_inst->seqNum);
                assert(ns_inst_it != nonSpecInsts.end());

                (*ns_inst_it).second = NULL;

                nonSpecInsts.erase(ns_inst_it);

                ++iqSquashedNonSpecRemoved;
            }

            // Might want to also clear out the head of the dependency graph.

            // Mark it as squashed within the IQ.
            squashed_inst->setSquashedInIQ();

            // @todo: Remove this hack where several statuses are set so the
            // inst will flow through the rest of the pipeline.
            squashed_inst->setIssued();
            squashed_inst->setCanCommit();
            squashed_inst->clearInIQ();

            //Update Thread IQ Count
            count[squashed_inst->threadNumber]--;

            ++freeEntries;

            DPRINTF(IQ, "[tid:%i]: Instruction [sn:%lli] PC %#x "
                    "squashed.\n",
                    tid, squashed_inst->seqNum, squashed_inst->readPC());
        }

        instList[tid].erase(squash_it--);
        ++iqSquashedInstsExamined;
    }
}

template <class Impl>
bool
InstructionQueue<Impl>::addToDependents(DynInstPtr &new_inst)
{
    // Loop through the instruction's source registers, adding
    // them to the dependency list if they are not ready.
    int8_t total_src_regs = new_inst->numSrcRegs();
    bool return_val = false;

    for (int src_reg_idx = 0;
         src_reg_idx < total_src_regs;
         src_reg_idx++)
    {
        // Only add it to the dependency graph if it's not ready.
        if (!new_inst->isReadySrcRegIdx(src_reg_idx)) {
            PhysRegIndex src_reg = new_inst->renamedSrcRegIdx(src_reg_idx);

            // Check the IQ's scoreboard to make sure the register
            // hasn't become ready while the instruction was in flight
            // between stages.  Only if it really isn't ready should
            // it be added to the dependency graph.
            if (src_reg >= numPhysRegs) {
                continue;
            } else if (regScoreboard[src_reg] == false) {
                DPRINTF(IQ, "Instruction PC %#x has src reg %i that "
                        "is being added to the dependency chain.\n",
                        new_inst->readPC(), src_reg);

                dependGraph.insert(src_reg, new_inst);

                // Change the return value to indicate that something
                // was added to the dependency graph.
                return_val = true;
            } else {
                DPRINTF(IQ, "Instruction PC %#x has src reg %i that "
                        "became ready before it reached the IQ.\n",
                        new_inst->readPC(), src_reg);
                // Mark a register ready within the instruction.
                new_inst->markSrcRegReady(src_reg_idx);
            }
        }
    }

    return return_val;
}

template <class Impl>
void
InstructionQueue<Impl>::addToProducers(DynInstPtr &new_inst)
{
    // Nothing really needs to be marked when an instruction becomes
    // the producer of a register's value, but for convenience a ptr
    // to the producing instruction will be placed in the head node of
    // the dependency links.
    int8_t total_dest_regs = new_inst->numDestRegs();

    for (int dest_reg_idx = 0;
         dest_reg_idx < total_dest_regs;
         dest_reg_idx++)
    {
        PhysRegIndex dest_reg = new_inst->renamedDestRegIdx(dest_reg_idx);

        // Instructions that use the misc regs will have a reg number
        // higher than the normal physical registers.  In this case these
        // registers are not renamed, and there is no need to track
        // dependencies as these instructions must be executed at commit.
        if (dest_reg >= numPhysRegs) {
            continue;
        }

        if (!dependGraph.empty(dest_reg)) {
            dependGraph.dump();
            panic("Dependency graph %i not empty!", dest_reg);
        }

        dependGraph.setInst(dest_reg, new_inst);

        // Mark the scoreboard to say it's not yet ready.
        regScoreboard[dest_reg] = false;
    }
}

template <class Impl>
void
InstructionQueue<Impl>::addIfReady(DynInstPtr &inst)
{
    // If the instruction now has all of its source registers
    // available, then add it to the list of ready instructions.
    if (inst->readyToIssue()) {

        //Add the instruction to the proper ready list.
        if (inst->isMemRef()) {

            DPRINTF(IQ, "Checking if memory instruction can issue.\n");

            // Message to the mem dependence unit that this instruction has
            // its registers ready.
            memDepUnit[inst->threadNumber].regsReady(inst);

            return;
        }

        OpClass op_class = inst->opClass();

        DPRINTF(IQ, "Instruction is ready to issue, putting it onto "
                "the ready list, PC %#x opclass:%i [sn:%lli].\n",
                inst->readPC(), op_class, inst->seqNum);

        readyInsts[op_class].push(inst);

        // Will need to reorder the list if either a queue is not on the list,
        // or it has an older instruction than last time.
        if (!queueOnList[op_class]) {
            addToOrderList(op_class);
        } else if (readyInsts[op_class].top()->seqNum  <
                   (*readyIt[op_class]).oldestInst) {
            listOrder.erase(readyIt[op_class]);
            addToOrderList(op_class);
        }
    }
}

template <class Impl>
int
InstructionQueue<Impl>::countInsts()
{
#if 0
    //ksewell:This works but definitely could use a cleaner write
    //with a more intuitive way of counting. Right now it's
    //just brute force ....
    // Change the #if if you want to use this method.
    int total_insts = 0;

    for (int i = 0; i < numThreads; ++i) {
        ListIt count_it = instList[i].begin();

        while (count_it != instList[i].end()) {
            if (!(*count_it)->isSquashed() && !(*count_it)->isSquashedInIQ()) {
                if (!(*count_it)->isIssued()) {
                    ++total_insts;
                } else if ((*count_it)->isMemRef() &&
                           !(*count_it)->memOpDone) {
                    // Loads that have not been marked as executed still count
                    // towards the total instructions.
                    ++total_insts;
                }
            }

            ++count_it;
        }
    }

    return total_insts;
#else
    return numEntries - freeEntries;
#endif
}

template <class Impl>
void
InstructionQueue<Impl>::dumpLists()
{
    for (int i = 0; i < Num_OpClasses; ++i) {
        cprintf("Ready list %i size: %i\n", i, readyInsts[i].size());

        cprintf("\n");
    }

    cprintf("Non speculative list size: %i\n", nonSpecInsts.size());

    NonSpecMapIt non_spec_it = nonSpecInsts.begin();
    NonSpecMapIt non_spec_end_it = nonSpecInsts.end();

    cprintf("Non speculative list: ");

    while (non_spec_it != non_spec_end_it) {
        cprintf("%#x [sn:%lli]", (*non_spec_it).second->readPC(),
                (*non_spec_it).second->seqNum);
        ++non_spec_it;
    }

    cprintf("\n");

    ListOrderIt list_order_it = listOrder.begin();
    ListOrderIt list_order_end_it = listOrder.end();
    int i = 1;

    cprintf("List order: ");

    while (list_order_it != list_order_end_it) {
        cprintf("%i OpClass:%i [sn:%lli] ", i, (*list_order_it).queueType,
                (*list_order_it).oldestInst);

        ++list_order_it;
        ++i;
    }

    cprintf("\n");
}


template <class Impl>
void
InstructionQueue<Impl>::dumpInsts()
{
    for (int i = 0; i < numThreads; ++i) {
        int num = 0;
        int valid_num = 0;
        ListIt inst_list_it = instList[i].begin();

        while (inst_list_it != instList[i].end())
        {
            cprintf("Instruction:%i\n",
                    num);
            if (!(*inst_list_it)->isSquashed()) {
                if (!(*inst_list_it)->isIssued()) {
                    ++valid_num;
                    cprintf("Count:%i\n", valid_num);
                } else if ((*inst_list_it)->isMemRef() &&
                           !(*inst_list_it)->memOpDone) {
                    // Loads that have not been marked as executed
                    // still count towards the total instructions.
                    ++valid_num;
                    cprintf("Count:%i\n", valid_num);
                }
            }

            cprintf("PC:%#x\n[sn:%lli]\n[tid:%i]\n"
                    "Issued:%i\nSquashed:%i\n",
                    (*inst_list_it)->readPC(),
                    (*inst_list_it)->seqNum,
                    (*inst_list_it)->threadNumber,
                    (*inst_list_it)->isIssued(),
                    (*inst_list_it)->isSquashed());

            if ((*inst_list_it)->isMemRef()) {
                cprintf("MemOpDone:%i\n", (*inst_list_it)->memOpDone);
            }

            cprintf("\n");

            inst_list_it++;
            ++num;
        }
    }

    cprintf("Insts to Execute list:\n");

    int num = 0;
    int valid_num = 0;
    ListIt inst_list_it = instsToExecute.begin();

    while (inst_list_it != instsToExecute.end())
    {
        cprintf("Instruction:%i\n",
                num);
        if (!(*inst_list_it)->isSquashed()) {
            if (!(*inst_list_it)->isIssued()) {
                ++valid_num;
                cprintf("Count:%i\n", valid_num);
            } else if ((*inst_list_it)->isMemRef() &&
                       !(*inst_list_it)->memOpDone) {
                // Loads that have not been marked as executed
                // still count towards the total instructions.
                ++valid_num;
                cprintf("Count:%i\n", valid_num);
            }
        }

        cprintf("PC:%#x\n[sn:%lli]\n[tid:%i]\n"
                "Issued:%i\nSquashed:%i\n",
                (*inst_list_it)->readPC(),
                (*inst_list_it)->seqNum,
                (*inst_list_it)->threadNumber,
                (*inst_list_it)->isIssued(),
                (*inst_list_it)->isSquashed());

        if ((*inst_list_it)->isMemRef()) {
            cprintf("MemOpDone:%i\n", (*inst_list_it)->memOpDone);
        }

        cprintf("\n");

        inst_list_it++;
        ++num;
    }
}
