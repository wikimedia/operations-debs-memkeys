#include "common.h"
#include "net/net.h"
#include "report/report.h"
#include "report/curses.h"

#include <vector>

namespace mckeys {

using namespace std;

CaptureEngine::CaptureEngine(const Config * config, const Pcap * session)
    : logger(Logger::getLogger("capture-engine")),
      config(config),
      session(session),
      barrier(new mqueue<Elem>()),
      stats(new Stats(config, barrier)),
      report(config->getReportType().makeReport(config, session, stats)),
      _is_terminated(false),
      queue_count(3),
      packets(),
      worker_threads(new thread[queue_count]),
      barrier_lock()
{
  packets.reserve(queue_count);
  for (int i = 0; i < queue_count; i++) {
    packets.insert(packets.begin() + i, new mqueue<Packet>());
    worker_threads[i] = thread(&CaptureEngine::processPackets, this, i, packets.at(i));
  }
  stats->start();
}

CaptureEngine::~CaptureEngine()
{
  if (isShutdown()) {
    logger->info(CONTEXT, "Capture engine successfully shut down");
  } else {
    logger->error(CONTEXT, "Capture engine not successfully shut down");
  }
  delete report;
  delete stats;
  delete barrier;
  // FIXME we should wrap these joins in a timer
  for (int i = 0; i < queue_count; i++) {
    worker_threads[i].join();
    logger->info(CONTEXT, "Worker thread %d dead", i);
  }
  while (!packets.empty()) {
    delete packets.back(), packets.pop_back();
  }
  delete[] worker_threads;
  delete logger;
}

void CaptureEngine::enqueue(const Packet& packet) {
#ifdef _DEVEL
  logger->trace(CONTEXT,
                "Produced packet: %ld", packet.id());
#endif
  packets.at(packet.id() % queue_count)->produce(packet);
}

bool CaptureEngine::isShutdown() const
{
  return (_is_terminated == true);
}

void CaptureEngine::shutdown()
{
  _is_terminated = true;
  stats->shutdown();
  report->shutdown();
}

///////////////////////////////////////////////////////////////////////////////
//                      Protected Methods                                    //
///////////////////////////////////////////////////////////////////////////////

/**
 * Process packets found in the packet queue.
 * This method pulls packets off of the packets queue, parses them into memcache
 * commands, and enqueues them via CaptureEngine::enqueue(MemcacheCommand) if
 * the parsed command is a response.
 *
 * This method is run in a thread and continues running until isShutdown()
 * returns true.
 *
 * @TODO create several worker threads to process packets in parallel
 * @protected
 */
void CaptureEngine::processPackets(int worker_id, mqueue<Packet>* work_queue) {
  static int64_t pktCount = 0;
  static llui_t resCount = 0;
  static bool isDebug = logger->isDebug();

  Backoff backoff;
  backoff.setMaxElapsedTimeMillis(10);
  backoff.setMaxIntervalMillis(10);
  uint64_t backoffMs = 0;
  struct timespec waitTime;

  logger->info(CONTEXT, "Worker %d starting capture processing", worker_id);

  while(!isShutdown()) {
    Packet packet;
    if (work_queue->consume(packet)) {
      pktCount += 1;
      if (backoffMs > 0) {
        backoffMs = 0;
        backoff.reset();
      }
#ifdef _DEVEL
      logger->trace(CONTEXT,
                    "worker %d Consumed packet %ld", worker_id, packet.id());
#endif
      MemcacheCommand mc = parse(packet);
      if (mc.isResponse()) {
        enqueue(mc);
        resCount += 1;
#ifdef _DEBUG
        logger->trace(CONTEXT,
                      "worker %d, packet %ld, key %s", worker_id, packet.id(),
                      mc.getObjectKey().c_str());
#endif
      } else {
#ifdef _DEVEL
        logger->trace(CONTEXT, "worker %d not a memcache response", worker_id);
#endif
      }
      if ((pktCount % 10000) == 0 && isDebug) {
        string out = getStatsString();
        uint64_t now = UtilTime::currentTimeMillis();
        llui_t tdiff = now - packet.timestamp();
        out.append(", memcache replies = ");
        out.append(to_string(resCount));
        out.append(", time diff = ");
        out.append(to_string(tdiff));
        logger->debug(out);
      }
    } else {
      backoffMs = backoff.getNextBackOffMillis();
      waitTime = UtilTime::millisToTimespec(backoffMs);
#ifdef _DEVEL
      logger->trace(CONTEXT,
                    "worker %d no packet to consume, will sleep %lu ms",
                    worker_id, backoffMs);
#endif
    }
    if (backoffMs > 0) {
      nanosleep(&waitTime, NULL);
    }
  }
  logger->info(CONTEXT, "Worker %d stopped processing packets", worker_id);
}

/**
 * Parse a packet into a memcache command.
 * @param Packet packet the packet to parse.
 * @return MemcacheCommand the command representation of the packet
 * @protected
 */
MemcacheCommand CaptureEngine::parse(const Packet& packet) const
{
  static const auto address = getIpAddress();
  return MemcacheCommand::create(packet, address);
}

/**
 * Enqueue a parsed memcache command for stats processing.
 * @param MemcacheCommand mc the command to enqueue
 * @protected
 */
void CaptureEngine::enqueue(const MemcacheCommand& mc)
{
  Elem e(mc.getObjectKey(), mc.getObjectSize());
#ifdef _DEBUG
  logger->trace(CONTEXT,
               "Produced stat: %s, %d", e.first.c_str(), e.second);
#endif
  barrier_lock.lock();
  barrier->produce(e);
  barrier_lock.unlock();
}

} // end namespace
