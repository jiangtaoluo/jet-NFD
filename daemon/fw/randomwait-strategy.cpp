/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2020-2022,  Chongqing University of Posts and TelecommunicationsK
 
 *
 * This file is my extension of NFD (Named Data Networking Forwarding Daemon).
 * Jiangtao Luo
 * 16 Mar 2020

 */

#include "randomwait-strategy.hpp"
#include "algorithm.hpp"
#include "core/logger.hpp"

#include "core/scheduler.hpp"
#include "core/random.hpp"

namespace nfd {
namespace fw {

NFD_REGISTER_STRATEGY(RandomWaitStrategy);

NFD_LOG_INIT(RandomWaitStrategy);

const time::milliseconds RandomWaitStrategy::RETX_SUPPRESSION_INITIAL(10);
const time::milliseconds RandomWaitStrategy::RETX_SUPPRESSION_MAX(250);

const time::microseconds RandomWaitStrategy::DELAY_MAX(3000); // 3 ms
const time::microseconds RandomWaitStrategy::DELAY_MIN(500); // 0.5 ms
  
const time::milliseconds RandomWaitStrategy::RETX_TIMER_UNIT(500); // 500ms per retx

  const uint32_t MAX_RETX_COUNT = 3; // maximum allowed retransmission

RandomWaitStrategy::RandomWaitStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder)
      , ProcessNackTraits(this)
  , m_retxSuppression(RETX_SUPPRESSION_INITIAL,
                      RetxSuppressionExponential::DEFAULT_MULTIPLIER,
                      RETX_SUPPRESSION_MAX)
 {
  ParsedInstanceName parsed = parseInstanceName(name);
  if (!parsed.parameters.empty()) {
    BOOST_THROW_EXCEPTION(std::invalid_argument("RandomWaitStrategy does not accept parameters"));
  }
  if (parsed.version && *parsed.version != getStrategyName()[-1].toVersion()) {
    BOOST_THROW_EXCEPTION(std::invalid_argument(
      "RandomWaitStrategy does not support version " + to_string(*parsed.version)));
  }
  this->setInstanceName(makeInstanceName(name, getStrategyName()));

}

const Name&
RandomWaitStrategy::getStrategyName()
{
  static Name strategyName("/localhost/nfd/strategy/random-wait/%FD%03");
  return strategyName;
}

void
RandomWaitStrategy::afterReceiveInterest(const Face& inFace, const Interest& interest,
                                        const shared_ptr<pit::Entry>& pitEntry)
{
 // Jiangtao Luo. 14 Feb 2020
  NFD_LOG_DEBUG("RandomWait slected for :"<<interest.getName() );
 
  // // Clone a new Interest. Jiangtao Luo. 18 Mar 2020
  // Block block = interest.wireEncode();
  // shared_ptr<Interest> pNewInterest = make_shared<Interest>(block);
  
  // int hopCount = interest.getHopCount();
  // pNewInterest->setHopCount(++hopCount); // increase hop count
  
  const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  const fib::NextHopList& nexthops = fibEntry.getNextHops();

  int nEligibleNextHops = 0;

  bool isSuppressed = false;

  for (const auto& nexthop : nexthops) {
    Face& outFace = nexthop.getFace();

    RetxSuppressionResult suppressResult = m_retxSuppression.decidePerUpstream(*pitEntry, outFace);

    if (suppressResult == RetxSuppressionResult::SUPPRESS) {
      NFD_LOG_DEBUG(interest << " from=" << inFace.getId()
                  << "to=" << outFace.getId() << " suppressed");
      isSuppressed = true;
      continue;
    }

    if ((outFace.getId() == inFace.getId() && outFace.getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC) ||
        wouldViolateScope(inFace, interest, outFace)) {
      continue;
    }

    if (inFace.getScope() ==  ndn::nfd::FACE_SCOPE_LOCAL || // from local
        outFace.getScope() == ndn::nfd::FACE_SCOPE_LOCAL) {  // out to local

      NFD_LOG_DEBUG(interest << " From/To local, send now. from=" << inFace.getId()
                           << " to=" << outFace.getId());
      
      this->sendInterest(pitEntry, outFace, interest);
      
      return;
    }

    // Receive from ad-hoc link
    //Send the new Interest with increased hopcount and random delay. Jiangtao Luo. 18 March 2020
    NFD_LOG_DEBUG(interest << " Out from=" << inFace.getId()
                           << " pitEntry-to=" << outFace.getId());

    this->sendInterestLater(outFace, interest, pitEntry);
    
    if (suppressResult == RetxSuppressionResult::FORWARD) {
      m_retxSuppression.incrementIntervalForOutRecord(*pitEntry->getOutRecord(outFace));
    }
    ++nEligibleNextHops;
  }

  if (nEligibleNextHops == 0 && !isSuppressed) {
    NFD_LOG_DEBUG(interest << " from=" << inFace.getId() << " noNextHop");

    lp::NackHeader nackHeader;
    nackHeader.setReason(lp::NackReason::NO_ROUTE);
    this->sendNack(pitEntry, inFace, nackHeader);

    this->rejectPendingInterest(pitEntry);

   }
}

void
RandomWaitStrategy::afterReceiveNack(const Face& inFace, const lp::Nack& nack,
                                    const shared_ptr<pit::Entry>& pitEntry)
{
  this->processNack(inFace, nack, pitEntry);
}

void
RandomWaitStrategy::sendInterestLater(Face& outFace, const Interest& interest,
                                      const shared_ptr<pit::Entry>& pitEntry)
{
  // 1ms -10ms
  std::uniform_int_distribution <uint64_t> dist( DELAY_MIN.count(), DELAY_MAX.count());
  time::microseconds delay = time::microseconds(dist(getGlobalRng()));

  NFD_LOG_DEBUG("RandomWaitStrategy::sendInterestLater for "
                << interest.getName().toUri()
                << " in " << delay);

  getForwarder().setRelayTimerForInterest(pitEntry, delay, outFace, interest);
}

void
RandomWaitStrategy::afterSendInterest(const shared_ptr<pit::Entry>& pitEntry, Face& outFace,
                                      const Interest& interest)
{
  // if allowed, schedule retransmission
  if (pitEntry->retxCount < MAX_RETX_COUNT) {
    time::milliseconds delay =
      time::milliseconds((++pitEntry->retxCount)*RETX_TIMER_UNIT);

    getForwarder().setRetxTimerForInterest(pitEntry, delay, outFace, interest);
    
  }
  else
    {
      this->onDroppedInterest(outFace, interest);
    }
}


} // namespace fw
} // namespace nfd
