/* augmentation.cc
   Jeremy Barnes, 1 March 2012
   Copyright (c) 2012 Datacratic.  All rights reserved.

   How we do auction augmentation.
*/

#include "augmentation_loop.h"
#include "jml/arch/timers.h"
#include "jml/arch/futex.h"
#include "jml/utils/vector_utils.h"
#include "jml/utils/set_utils.h"
#include "jml/arch/exception_handler.h"
#include "soa/service/zmq_utils.h"
#include <iostream>
#include <boost/make_shared.hpp>
#include "rtbkit/core/agent_configuration/agent_config.h"


using namespace std;
using namespace ML;



namespace RTBKIT {


/*****************************************************************************/
/* AUGMENTATION LOOP                                                         */
/*****************************************************************************/

AugmentationLoop::
AugmentationLoop(ServiceBase & parent,
                 const std::string & name)
    : ServiceBase(name, parent),
      allAugmentors(0),
      idle_(1),
      inbox(65536),
      disconnections(1024)
{
    updateAllAugmentors();
}

AugmentationLoop::
AugmentationLoop(std::shared_ptr<ServiceProxies> proxies,
                 const std::string & name)
    : ServiceBase(name, proxies),
      allAugmentors(0),
      idle_(1),
      inbox(65536),
      disconnections(1024)
{
    updateAllAugmentors();
}

AugmentationLoop::
~AugmentationLoop()
{
}

void
AugmentationLoop::
init(const Json::Value& conf)
{

    augmentorInterface = AugmentorInterface::create(
            serviceName() + ".augmentor", getServices(), conf);

    augmentorInterface->onConnection = [=](
            std::string&& name, std::shared_ptr<AugmentorInstanceInfo>&& instance) {
        doConnection(std::move(name), std::move(instance));
    };

    // These events show up on the zookeeper thread so redirect them to our
    // message loop thread.
    augmentorInterface->onDisconnection = [=](const std::string& client) {
        disconnections.push(client);
    };

    disconnections.onEvent = [&] (const std::string& addr)
        {
            doDisconnection(addr);
        };


    augmentorInterface->onResponse = [=](AugmentationResponse&& response) {
        doResponse(std::move(response));
    };

    augmentorInterface->init();

    inbox.onEvent = [&] (std::shared_ptr<Entry>&& entry)
        {
            doAugmentation(std::move(entry));
        };

    addSource("AugmentationLoop::inbox", inbox);
    addSource("AugmentationLoop::disconnections", disconnections);

    addPeriodic("AugmentationLoop::checkExpiries", 0.001,
                [=] (int) { checkExpiries(); });

    addPeriodic("AugmentationLoop::recordStats", 0.977,
                [=] (int) { recordStats(); });
}

void
AugmentationLoop::
start()
{
    augmentorInterface->start();
    MessageLoop::start();
}

void
AugmentationLoop::
sleepUntilIdle()
{
    while (!idle_)
        futex_wait(idle_, 0);
}

void
AugmentationLoop::
shutdown()
{
    MessageLoop::shutdown();
    augmentorInterface->shutdown();
}

size_t
AugmentationLoop::
numAugmenting() const
{
    return augmenting.size();
}

void
AugmentationLoop::
bindAugmentors(const std::string & uri)
{
    try {
       // toAugmentors.bind(uri.c_str());
    } catch (const std::exception & exc) {
        throw Exception("error while binding augmentation URI %s: %s",
                        uri.c_str(), exc.what());
    }
}

void
AugmentationLoop::
recordStats()
{
    for (auto it = augmentors.begin(), end = augmentors.end();
         it != end;  ++it)
    {
        size_t inFlights = 0;
        for (const auto& instance : it->second->instances)
            inFlights += instance->numInFlight;

        recordLevel(inFlights, "augmentor.%s.numInFlight", it->first);
    }
}


void
AugmentationLoop::
checkExpiries()
{
    Date now = Date::now();

    auto onExpired = [&] (const Id & id,
                          const std::shared_ptr<Entry> & entry) -> Date
        {
            for (auto it = entry->outstanding.begin(),
                     end = entry->outstanding.end();
                 it != end; ++it)
            {
                recordHit("augmentor.%s.expiredTooLate", *it);
            }
                
            this->augmentationExpired(id, *entry);
            return Date();
        };

    if (augmenting.earliest <= now)
        augmenting.expire(onExpired, now);

    if (augmenting.empty() && !idle_) {
        idle_ = 1;
        futex_wake(idle_);
    }

}

// Is not thread safe and should only be called from the polling loop thread.
void
AugmentationLoop::
updateAllAugmentors()
{
    unique_ptr<AllAugmentorInfo> newInfo(new AllAugmentorInfo());

    for (auto it = augmentors.begin(), end = augmentors.end(); it != end;  ++it) {
        ExcAssert(it->second);

        AugmentorInfo & aug = *it->second;
        ExcAssert(!aug.name.empty());

        AugmentorInfoEntry entry;
        entry.name = aug.name;
        entry.info = it->second;
        newInfo->push_back(entry);
    }

    std::sort(newInfo->begin(), newInfo->end(),
            [] (const AugmentorInfoEntry & entry1,
                const AugmentorInfoEntry & entry2)
            {
                return entry1.name < entry2.name;
            });
    
    // Make sure our struct is fully written before we make it visible.
    ML::memory_barrier();

    AllAugmentorInfo * current = allAugmentors;
    allAugmentors = newInfo.get();
    newInfo.release();
    if (current)
        allAugmentorsGc.defer([=] () { delete current; });
}

void
AugmentationLoop::
augment(const std::shared_ptr<AugmentationInfo> & info,
        Date timeout,
        const OnFinished & onFinished)
{
    Date now = Date::now();

    auto entry = std::make_shared<Entry>();
    entry->onFinished = onFinished;
    entry->info = info;
    entry->timeout = timeout;

    // Get a set of all augmentors
    std::set<std::string> augmentors;

    // Now go through and find all of the bidders
    for (unsigned i = 0;  i < info->potentialGroups.size();  ++i) {
        const GroupPotentialBidders & group = info->potentialGroups[i];
        for (unsigned j = 0;  j < group.size();  ++j) {
            const PotentialBidder & bidder = group[j];
            const AgentConfig & config = *bidder.config;
            for (unsigned k = 0;  k < config.augmentations.size();  ++k) {
                const std::string & name = config.augmentations[k].name;
                augmentors.insert(name);
                entry->augmentorAgents[name].insert(bidder.agent);
            }
        }
    }

    //cerr << "need augmentors " << augmentors << endl;

    // Find which ones are actually available...
    GcLock::SharedGuard guard(allAugmentorsGc);
    const AllAugmentorInfo * ai = allAugmentors;
    
    ExcAssert(ai);

    auto it1 = augmentors.begin(), end1 = augmentors.end();
    auto it2 = ai->begin(), end2 = ai->end();

    while (it1 != end1 && it2 != end2) {
        if (*it1 == it2->name) {
            // Augmentor we need to run
            //cerr << "augmenting with " << it2->name << endl;
            recordEvent("augmentation.request");
            string eventName = "augmentor." + it2->name + ".request";
            recordEvent(eventName.c_str());
            
             entry->outstanding.insert(*it1);

            ++it1;
            ++it2;
        }
        else if (*it1 < it2->name) {
            // Augmentor is not available
            //cerr << "augmentor " << *it1 << " is not available" << endl;
            ++it1;
        }
        else if (it2->name < *it1) {
            // Augmentor is not required
            //cerr << "augmentor " << it2->name << " is not required" << endl;
            ++it2;
        }
        else throw ML::Exception("logic error traversing augmentors");
    }

#if 0
    while (it1 != end1) {
        cerr << "augmentor " << *it1 << " is not available" << endl;
        ++it1;
    }
    
    while (it2 != end2) {
        cerr << "augmentor " << it2->name << " is not required" << endl;
        ++it2;
    }
#endif

    if (entry->outstanding.empty()) {
        // No augmentors required... run the auction straight away
        onFinished(info);
    }
    else {
        //cerr << "putting in inbox" << endl;
        inbox.push(entry);
    }
}

std::shared_ptr<AugmentorInstanceInfo>
AugmentationLoop::
pickInstance(AugmentorInfo& aug)
{
    std::shared_ptr<AugmentorInstanceInfo> instance;
    int minInFlights = std::numeric_limits<int>::max();

    stringstream ss;

    for (auto it = aug.instances.begin(), end = aug.instances.end();
         it != end; ++it)
    {
        auto & ptr = *it;
        if (ptr->numInFlight >= minInFlights) continue;
        if (ptr->numInFlight >= ptr->maxInFlight) continue;

        instance = ptr;
        minInFlights = ptr->numInFlight;
    }

    if (instance) instance->numInFlight++;
    return instance;
}


void
AugmentationLoop::
doAugmentation(std::shared_ptr<Entry>&& entry)
{
    Date now = Date::now();

    if (augmenting.count(entry->info->auction->id)) {
        stringstream ss;
        ss << "AugmentationLoop: duplicate auction id detected "
            << entry->info->auction->id << endl;
        cerr << ss.str();
        recordHit("duplicateAuction");
        return;
    }

    bool sentToAugmentor = false;

    for (auto it = entry->outstanding.begin(), end = entry->outstanding.end();
         it != end;  ++it)
    {
        auto & aug = *augmentors[*it];

        auto instance = pickInstance(aug);
        if (!instance) {
            recordHit("augmentor.%s.skippedTooManyInFlight", *it);
            continue;
        }
        recordHit("augmentor.%s.instances.%s.request", *it, instance->address());

        set<string> agents = entry->augmentorAgents[*it];

        entry->instances[*it] = instance;
        
        // Send the message to the augmentor
        augmentorInterface->sendAugmentMessage(
                instance,
                *it,
                entry->info->auction,
                agents,
                Date::now());

        sentToAugmentor = true;
    }

    if (sentToAugmentor)
        augmenting.insert(entry->info->auction->id, std::move(entry), entry->timeout);
    else entry->onFinished(entry->info);

    recordLevel(Date::now().secondsSince(now), "requestTimeMs");

    idle_ = 0;
}

void
AugmentationLoop::
doConnection(std::string&& name, std::shared_ptr<AugmentorInstanceInfo>&& instance)
{
    //cerr << "configuring augmentor " << name << " on " << connectTo
    //     << endl;

    doDisconnection(instance->address(), name);

    auto addr = instance->address();
    auto& info = augmentors[name];
    if (!info) {
        info = std::make_shared<AugmentorInfo>(name);
        recordHit("augmentor.%s.configured", name);
    }

    info->instances.push_back(std::move(instance));
    recordHit("augmentor.%s.instances.%s.configured", name, addr);


    updateAllAugmentors();

}


/** Note that there's a race here between the disconnection zk event and the
    config message sent by the same service reconnecting.

    This should be pretty rare and requires heartbeats which are problematic
    over zmq so, for the moment, we'll leave it as is.
 */
void
AugmentationLoop::
doDisconnection(const std::string & addr, const std::string & aug)
{
    std::vector<std::string> toErase;

    for (auto& info: augmentors) {
        auto& augmentor = *info.second;
        if (!augmentor.name.empty() && augmentor.name != aug) continue;

        for (auto it = augmentor.instances.begin(),
                 end = augmentor.instances.end();
             it != end; ++it)
        {
            auto & ptr = *it;
            if (ptr->address() != addr) continue;

            recordHit("augmentor.%s.instances.%s.disconnected",
                    augmentor.name, ptr->address());

            augmentor.instances.erase(it);
            break;
        }

        // Erasing would invalidate our iterator so need to defer it.
        if (augmentor.instances.empty())
            toErase.push_back(augmentor.name);
    }

    // We let the inFlight auctions expire naturally.
    for (const auto& name : toErase)
        augmentors.erase(name);

    if (!toErase.empty())
        updateAllAugmentors();
}


void
AugmentationLoop::
doResponse(AugmentationResponse&& response)
{
    recordEvent("augmentation.response");
    ML::Timer timer;

    auto augmentation = response.augmentation;
    auto augmentor = response.augmentor;

    AugmentationList augmentationList;
    if (augmentation != "" && augmentation != "null") {
        try {
            Json::Value augmentationJson;

            JML_TRACE_EXCEPTIONS(false);
            augmentationJson = Json::parse(augmentation);
            augmentationList = AugmentationList::fromJson(augmentationJson);
        } catch (const std::exception & exc) {
            string eventName = "augmentor." + response.augmentor
                + ".responseParsingExceptions";
            recordEvent(eventName.c_str(), ET_COUNT);
        }
    }

    recordLevel(timer.elapsed_wall(), "responseParseTimeMs");

    {
        double timeTakenMs = response.startTime.secondsUntil(Date::now()) * 1000.0;
        string eventName = "augmentor." + augmentor + ".timeTakenMs";
        recordEvent(eventName.c_str(), ET_OUTCOME, timeTakenMs);
    }

    {
        double responseLength = augmentation.size();
        string eventName = "augmentor." + augmentor + ".responseLengthBytes";
        recordEvent(eventName.c_str(), ET_OUTCOME, responseLength);
    }

    auto augmentorIt = augmentors.find(augmentor);
    if (augmentorIt != augmentors.end()) {
        auto instance = augmentorIt->second->findInstance(response.addr);
        if (instance) instance->numInFlight--;
    }

    auto augmentingIt = augmenting.find(response.auctionId);
    if (augmentingIt == augmenting.end()) {
        recordHit("augmentation.unknown");
        recordHit("augmentor.%s.unknown", augmentor);
        recordHit("augmentor.%s.instances.%s.unknown", augmentor, response.addr);
        return;
    }

    auto& entry = *augmentingIt;

    const char* eventType =
        (augmentation == "" || augmentation == "null") ?
        "nullResponse" : "validResponse";
    recordHit("augmentor.%s.%s", augmentor, eventType);
    recordHit("augmentor.%s.instances.%s.%s", augmentor, response.addr, eventType);

    auto& auctionAugs = entry.second->info->auction->augmentations;
    auctionAugs[augmentor].mergeWith(augmentationList);

    entry.second->outstanding.erase(augmentor);
    if (entry.second->outstanding.empty()) {
        entry.second->onFinished(entry.second->info);
        augmenting.erase(augmentingIt);
    }
}

void
AugmentationLoop::
augmentationExpired(const Id & id, const Entry & entry)
{
    for (const auto & instance: entry.instances) {
        // If the instance still exsits (it is still alive), we decrement
        // the inFlight count
        auto info = instance.second.lock();
        if (info) info->numInFlight--;
    }

    entry.onFinished(entry.info);
}                     

} // namespace RTBKIT
