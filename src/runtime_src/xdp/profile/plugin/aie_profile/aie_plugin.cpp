/**
 * Copyright (C) 2020 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#define XDP_SOURCE

#include "xdp/profile/plugin/vp_base/info.h"
#include "xdp/profile/plugin/aie_profile/aie_plugin.h"
#include "xdp/profile/writer/aie_profile/aie_writer.h"

#include "core/common/message.h"
#include "core/common/system.h"
#include "core/common/time.h"
#include "core/common/config_reader.h"
#include "core/include/experimental/xrt-next.h"
#include "core/edge/user/shim.h"
#include "xdp/profile/database/database.h"

#include <boost/algorithm/string.hpp>

#define NUM_CORE_COUNTERS   4
#define NUM_MEMORY_COUNTERS 2
#define BASE_MEMORY_COUNTER 128

#define GROUP_DMA_MASK                   0x0000f000
#define GROUP_LOCK_MASK                  0x55555555
#define GROUP_CONFLICT_MASK              0x000000ff
#define GROUP_ERROR_MASK                 0x00003fff
#define GROUP_STREAM_SWITCH_IDLE_MASK    0x11111111
#define GROUP_STREAM_SWITCH_RUNNING_MASK 0x22222222
#define GROUP_STREAM_SWITCH_STALLED_MASK 0x44444444
#define GROUP_STREAM_SWITCH_TLAST_MASK   0x88888888
#define GROUP_CORE_PROGRAM_FLOW_MASK     0x00001FE0
#define GROUP_CORE_STALL_MASK            0x0000000F

namespace {
  static void* fetchAieDevInst(void* devHandle)
  {
    auto drv = ZYNQ::shim::handleCheck(devHandle);
    if (!drv)
      return nullptr ;
    auto aieArray = drv->getAieArray() ;
    if (!aieArray)
      return nullptr ;
    return aieArray->getDevInst() ;
  }

  static void* allocateAieDevice(void* devHandle)
  {
    XAie_DevInst* aieDevInst =
      static_cast<XAie_DevInst*>(fetchAieDevInst(devHandle)) ;
    if (!aieDevInst)
      return nullptr ;
    return new xaiefal::XAieDev(aieDevInst, false) ;
  }

  static void deallocateAieDevice(void* aieDevice)
  {
    xaiefal::XAieDev* object = static_cast<xaiefal::XAieDev*>(aieDevice) ;
    if (object != nullptr)
      delete object ;
  }

} // end anonymous namespace

namespace xdp {
  using severity_level = xrt_core::message::severity_level;

  AIEProfilingPlugin::AIEProfilingPlugin() 
      : XDPPlugin()
  {
    db->registerPlugin(this);
    db->registerInfo(info::aie_profile);
    getPollingInterval();

    //
    // Pre-defined metric sets
    //
    // **** Core Module Counters ****
    mCoreStartEvents = {
      {"heat_map",              {XAIE_EVENT_ACTIVE_CORE,               XAIE_EVENT_GROUP_CORE_STALL_CORE,
                                 XAIE_EVENT_INSTR_VECTOR_CORE,         XAIE_EVENT_GROUP_CORE_PROGRAM_FLOW_CORE}},
      {"stalls",                {XAIE_EVENT_MEMORY_STALL_CORE,         XAIE_EVENT_STREAM_STALL_CORE,
                                 XAIE_EVENT_LOCK_STALL_CORE,           XAIE_EVENT_CASCADE_STALL_CORE}},
      {"execution",             {XAIE_EVENT_INSTR_VECTOR_CORE,         XAIE_EVENT_INSTR_LOAD_CORE,
                                 XAIE_EVENT_INSTR_STORE_CORE,          XAIE_EVENT_GROUP_CORE_PROGRAM_FLOW_CORE}},
      {"floating_point",        {XAIE_EVENT_FP_OVERFLOW_CORE,          XAIE_EVENT_FP_UNDERFLOW_CORE,
                                 XAIE_EVENT_FP_INVALID_CORE,           XAIE_EVENT_FP_DIV_BY_ZERO_CORE}},
      {"stream_put_get",        {XAIE_EVENT_INSTR_CASCADE_GET_CORE,    XAIE_EVENT_INSTR_CASCADE_PUT_CORE,
                                 XAIE_EVENT_INSTR_STREAM_GET_CORE,     XAIE_EVENT_INSTR_STREAM_PUT_CORE}},
      {"stream_switch_idle",    {XAIE_EVENT_GROUP_STREAM_SWITCH_CORE,  XAIE_EVENT_PORT_IDLE_0_CORE,
                                 XAIE_EVENT_PORT_IDLE_1_CORE,          XAIE_EVENT_PORT_IDLE_2_CORE}},
      {"stream_switch_running", {XAIE_EVENT_GROUP_STREAM_SWITCH_CORE,  XAIE_EVENT_PORT_RUNNING_0_CORE,
                                 XAIE_EVENT_PORT_RUNNING_1_CORE,       XAIE_EVENT_PORT_RUNNING_2_CORE}},
      {"stream_switch_stalled", {XAIE_EVENT_GROUP_STREAM_SWITCH_CORE,  XAIE_EVENT_PORT_STALLED_0_CORE,
                                 XAIE_EVENT_PORT_STALLED_1_CORE,       XAIE_EVENT_PORT_STALLED_2_CORE}},
      {"stream_switch_tlast",   {XAIE_EVENT_GROUP_STREAM_SWITCH_CORE,  XAIE_EVENT_PORT_TLAST_0_CORE,
                                 XAIE_EVENT_PORT_TLAST_1_CORE,         XAIE_EVENT_PORT_TLAST_2_CORE}}
    };
    mCoreEndEvents = {
      {"heat_map",              {XAIE_EVENT_ACTIVE_CORE,               XAIE_EVENT_GROUP_CORE_STALL_CORE,
                                 XAIE_EVENT_INSTR_VECTOR_CORE,         XAIE_EVENT_GROUP_CORE_PROGRAM_FLOW_CORE}},
      {"stalls",                {XAIE_EVENT_MEMORY_STALL_CORE,         XAIE_EVENT_STREAM_STALL_CORE,
                                 XAIE_EVENT_LOCK_STALL_CORE,           XAIE_EVENT_CASCADE_STALL_CORE}},
      {"execution",             {XAIE_EVENT_INSTR_VECTOR_CORE,         XAIE_EVENT_INSTR_LOAD_CORE,
                                 XAIE_EVENT_INSTR_STORE_CORE,          XAIE_EVENT_GROUP_CORE_PROGRAM_FLOW_CORE}},
      {"floating_point",        {XAIE_EVENT_FP_OVERFLOW_CORE,          XAIE_EVENT_FP_UNDERFLOW_CORE,
                                 XAIE_EVENT_FP_INVALID_CORE,           XAIE_EVENT_FP_DIV_BY_ZERO_CORE}},
      {"stream_put_get",        {XAIE_EVENT_INSTR_CASCADE_GET_CORE,    XAIE_EVENT_INSTR_CASCADE_PUT_CORE,
                                 XAIE_EVENT_INSTR_STREAM_GET_CORE,     XAIE_EVENT_INSTR_STREAM_PUT_CORE}},
      {"stream_switch_idle",    {XAIE_EVENT_GROUP_STREAM_SWITCH_CORE,  XAIE_EVENT_PORT_IDLE_0_CORE,
                                 XAIE_EVENT_PORT_IDLE_1_CORE,          XAIE_EVENT_PORT_IDLE_2_CORE}},
      {"stream_switch_running", {XAIE_EVENT_GROUP_STREAM_SWITCH_CORE,  XAIE_EVENT_PORT_RUNNING_0_CORE,
                                 XAIE_EVENT_PORT_RUNNING_1_CORE,       XAIE_EVENT_PORT_RUNNING_2_CORE}},
      {"stream_switch_stalled", {XAIE_EVENT_GROUP_STREAM_SWITCH_CORE,  XAIE_EVENT_PORT_STALLED_0_CORE,
                                 XAIE_EVENT_PORT_STALLED_1_CORE,       XAIE_EVENT_PORT_STALLED_2_CORE}},
      {"stream_switch_tlast",   {XAIE_EVENT_GROUP_STREAM_SWITCH_CORE,  XAIE_EVENT_PORT_TLAST_0_CORE,
                                 XAIE_EVENT_PORT_TLAST_1_CORE,         XAIE_EVENT_PORT_TLAST_2_CORE}}
    };

    // **** Memory Module Counters ****
    mMemoryStartEvents = {
      {"conflicts",             {XAIE_EVENT_GROUP_MEMORY_CONFLICT_MEM, XAIE_EVENT_GROUP_ERRORS_MEM}},
      {"dma_locks",             {XAIE_EVENT_GROUP_DMA_ACTIVITY_MEM,    XAIE_EVENT_GROUP_LOCK_MEM}},
      {"dma_stalls_s2mm",       {XAIE_EVENT_DMA_S2MM_0_STALLED_LOCK_ACQUIRE_MEM,
                                 XAIE_EVENT_DMA_S2MM_1_STALLED_LOCK_ACQUIRE_MEM}},
      {"dma_stalls_mm2s",       {XAIE_EVENT_DMA_MM2S_0_STALLED_LOCK_ACQUIRE_MEM,
                                 XAIE_EVENT_DMA_MM2S_1_STALLED_LOCK_ACQUIRE_MEM}}
    };
    mMemoryEndEvents = {
      {"conflicts",             {XAIE_EVENT_GROUP_MEMORY_CONFLICT_MEM, XAIE_EVENT_GROUP_ERRORS_MEM}},
      {"dma_locks",             {XAIE_EVENT_GROUP_DMA_ACTIVITY_MEM,    XAIE_EVENT_GROUP_LOCK_MEM}}, 
      {"dma_stalls_s2mm",       {XAIE_EVENT_DMA_S2MM_0_STALLED_LOCK_ACQUIRE_MEM,
                                 XAIE_EVENT_DMA_S2MM_1_STALLED_LOCK_ACQUIRE_MEM}},
      {"dma_stalls_mm2s",       {XAIE_EVENT_DMA_MM2S_0_STALLED_LOCK_ACQUIRE_MEM,
                                 XAIE_EVENT_DMA_MM2S_1_STALLED_LOCK_ACQUIRE_MEM}}
    };

    // String event values for guidance and output
    mCoreEventStrings = {
      {"heat_map",              {"ACTIVE_CORE",               "GROUP_CORE_STALL_CORE",
                                 "INSTR_VECTOR_CORE",         "GROUP_CORE_PROGRAM_FLOW"}},
      {"stalls",                {"MEMORY_STALL_CORE",         "STREAM_STALL_CORE",
                                 "LOCK_STALL_CORE",           "CASCADE_STALL_CORE"}},
      {"execution",             {"INSTR_VECTOR_CORE",         "INSTR_LOAD_CORE",
                                 "INSTR_STORE_CORE",          "GROUP_CORE_PROGRAM_FLOW"}},
      {"floating_point",        {"FP_OVERFLOW_CORE",          "FP_UNDERFLOW_CORE",
                                 "FP_INVALID_CORE",           "FP_DIV_BY_ZERO_CORE"}},
      {"stream_put_get",        {"INSTR_CASCADE_GET_CORE",    "INSTR_CASCADE_PUT_CORE",
                                 "INSTR_STREAM_GET_CORE",     "INSTR_STREAM_PUT_CORE"}},
      {"stream_switch_idle",    {"GROUP_STREAM_SWITCH_CORE",  "PORT_IDLE_0_CORE",
                                 "PORT_IDLE_1_CORE",          "PORT_IDLE_2_CORE"}},
      {"stream_switch_running", {"GROUP_STREAM_SWITCH_CORE",  "PORT_RUNNING_0_CORE",
                                 "PORT_RUNNING_1_CORE",       "PORT_RUNNING_2_CORE"}},
      {"stream_switch_stalled", {"GROUP_STREAM_SWITCH_CORE",  "PORT_STALLED_0_CORE",
                                 "PORT_STALLED_1_CORE",       "PORT_STALLED_2_CORE"}},
      {"stream_switch_tlast",   {"GROUP_STREAM_SWITCH_CORE",  "PORT_TLAST_0_CORE",
                                 "PORT_TLAST_1_CORE",         "PORT_TLAST_2_CORE"}}
    };
    mMemoryEventStrings = {
      {"conflicts",             {"GROUP_MEMORY_CONFLICT_MEM", "GROUP_ERRORS_MEM"}},
      {"dma_locks",             {"GROUP_DMA_ACTIVITY_MEM",    "GROUP_LOCK_MEM"}},
      {"dma_stalls_s2mm",       {"DMA_S2MM_0_STALLED_LOCK_ACQUIRE_MEM",
                                 "DMA_S2MM_1_STALLED_LOCK_ACQUIRE_MEM"}},
      {"dma_stalls_mm2s",       {"DMA_MM2S_0_STALLED_LOCK_ACQUIRE_MEM",
                                 "DMA_MM2S_1_STALLED_LOCK_ACQUIRE_MEM"}}
    };
  }

  AIEProfilingPlugin::~AIEProfilingPlugin()
  {
    // Stop the polling thread
    endPoll();

    if (VPDatabase::alive()) {
      for (auto w : writers) {
        w->write(false);
      }

      db->unregisterPlugin(this);
    }
  }

  void AIEProfilingPlugin::getPollingInterval()
  {
    // Get polling interval (in usec; minimum is 100)
    mPollingInterval = xrt_core::config::get_aie_profile_interval_us();
    //if (mPollingInterval < 100) {
    //  mPollingInterval = 100;
    //  xrt_core::message::send(severity_level::warning, "XRT", 
    //      "Minimum supported AIE profile interval is 100 usec.");
    //}
  }

  void AIEProfilingPlugin::printTileModStats(xaiefal::XAieDev* aieDevice, const tile_type& tile, bool isCore)
  {
    auto col = tile.col;
    auto row = tile.row + 1;
    auto loc = XAie_TileLoc(col, row);
    std::string modType = isCore ? "Core" : "Memory";
    XAie_ModuleType mod = isCore ? XAIE_CORE_MOD :  XAIE_MEM_MOD;
    std::stringstream msg;

    const std::string groups[3] = {
      XAIEDEV_DEFAULT_GROUP_GENERIC,
      XAIEDEV_DEFAULT_GROUP_STATIC,
      XAIEDEV_DEFAULT_GROUP_AVAIL
    };

    msg << "Resource usage stats for Tile : (" << col << "," << row << ") Module : " << modType << std::endl;
    for (auto&g : groups) {
      auto stats = aieDevice->getRscStat(g);
      auto pc = stats.getNumRsc(loc, mod, XAIE_PERFCNT_RSC);
      auto ts = stats.getNumRsc(loc, mod, xaiefal::XAIE_TRACE_EVENTS_RSC);
      auto bc = stats.getNumRsc(loc, mod, XAIE_BCAST_CHANNEL_RSC);
      msg << "Resource Group : " << std::left <<  std::setw(10) << g << " "
          << "Performance Counters : " << pc << " "
          << "Trace Slots : " << ts << " "
          << "Broadcast Channels : " << bc << " "
          << std::endl;
    }

    xrt_core::message::send(severity_level::info, "XRT", msg.str());
  }

  uint32_t AIEProfilingPlugin::getNumFreeCtr(xaiefal::XAieDev* aieDevice,
                                          const std::vector<tile_type>& tiles,
                                          bool isCore,
                                          const std::string& metricSet)
  {
    uint32_t numFreeCtr = 0;
    uint32_t tileId = 0;
    XAie_ModuleType mod = isCore ? XAIE_CORE_MOD : XAIE_MEM_MOD ;
    auto stats = aieDevice->getRscStat(XAIEDEV_DEFAULT_GROUP_AVAIL);

    for (unsigned int i=0; i < tiles.size(); i++) {
      auto loc = XAie_TileLoc(tiles[i].col, tiles[i].row + 1);
      auto avail = stats.getNumRsc(loc, mod, XAIE_PERFCNT_RSC);
      if (i == 0) {
        numFreeCtr = avail;
      } else {
        if (avail < numFreeCtr) {
          numFreeCtr = avail;
          tileId = i;
        }
      }
    }

    auto& requestedEvents = isCore ? mCoreStartEvents[metricSet] : mMemoryStartEvents[metricSet];
    auto& EventStrings =    isCore ? mCoreEventStrings[metricSet] : mMemoryEventStrings[metricSet];

    auto numTotalEvents = requestedEvents.size();
    if (numFreeCtr < numTotalEvents) {
      std::stringstream msg;
      std::string modType = isCore ? "core" : "memory";
      msg << "Only " << numFreeCtr << " out of " << numTotalEvents
          << " metrics were available for aie "
          << modType <<  " module profiling due to resource constraints. "
          << "AIE profiling uses performance counters which could be already used by AIE trace, ECC, etc."
          << std::endl;

      msg << "Available metrics : ";
      for (unsigned int i=0; i < numFreeCtr; i++) {
        msg << EventStrings[i] << " ";
      }
      msg << std::endl;

      msg << "Unavailable metrics : ";
      for (unsigned int i=numFreeCtr; i < numTotalEvents; i++) {
        msg << EventStrings[i] << " ";
      }

      xrt_core::message::send(severity_level::warning, "XRT", msg.str());
      printTileModStats(aieDevice, tiles[tileId], isCore);
    }

    return numFreeCtr;
  }

  bool AIEProfilingPlugin::setMetrics(uint64_t deviceId, void* handle)
  {
    XAie_DevInst* aieDevInst =
      static_cast<XAie_DevInst*>(db->getStaticInfo().getAieDevInst(fetchAieDevInst, handle)) ;
    xaiefal::XAieDev* aieDevice =
      static_cast<xaiefal::XAieDev*>(db->getStaticInfo().getAieDevice(allocateAieDevice, deallocateAieDevice, handle)) ;
    if (!aieDevInst || !aieDevice) {
      xrt_core::message::send(severity_level::warning, "XRT", 
          "Unable to get AIE device. There will be no AIE profiling.");
      return false;
    }

    bool runtimeCounters = false;
	  std::shared_ptr<xrt_core::device> device = xrt_core::get_userpf_device(handle);

    // Get AIE clock frequency
    auto clockFreqMhz = xrt_core::edge::aie::get_clock_freq_mhz(device.get());

    // Configure both core and memory module counters
    for (int module=0; module < 2; ++module) {
      bool isCore = (module == 0);

      std::string metricsStr = isCore ?
          xrt_core::config::get_aie_profile_core_metrics() :
          xrt_core::config::get_aie_profile_memory_metrics();
      if (metricsStr.empty())
        continue;

      std::vector<std::string> vec;
      boost::split(vec, metricsStr, boost::is_any_of(":"));

      for (int i=0; i < vec.size(); ++i) {
        boost::replace_all(vec.at(i), "{", "");
        boost::replace_all(vec.at(i), "}", "");
      }
      
      // Determine specification type based on vector size:
      //   * Size = 1: All tiles
      //     * aie_profile_core_metrics = <heat_map|stalls|execution>
      //     * aie_profile_memory_metrics = <dma_locks|conflicts>
      //   * Size = 2: Single tile
      //     * aie_profile_core_metrics = {<column>,<row>}:<heat_map|stalls|execution>
      //     * aie_profile_memory_metrics = {<column>,<row>}:<dma_locks|conflicts>
      //   * Size = 3: Range of tiles
      //     * aie_profile_core_metrics = {<mincolumn,<minrow>}:{<maxcolumn>,<maxrow>}:<heat_map|stalls|execution>
      //     * aie_profile_memory_metrics = {<mincolumn,<minrow>}:{<maxcolumn>,<maxrow>}:<dma_locks|conflicts>
      std::string metricSet  = vec.at( vec.size()-1 );
      std::string moduleName = isCore ? "core" : "memory";

      // Ensure requested metric set is supported (if not, use default)
      if ((isCore && (mCoreStartEvents.find(metricSet) == mCoreStartEvents.end()))
          || (!isCore && (mMemoryStartEvents.find(metricSet) == mMemoryStartEvents.end()))) {
        std::string defaultSet = isCore ? "heat_map" : "conflicts";
        std::stringstream msg;
        msg << "Unable to find " << moduleName << " metric set " << metricSet 
            << ". Using default of " << defaultSet << ".";
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        metricSet = defaultSet;
      }

      // Compile list of tiles based on how its specified in setting
      std::vector<tile_type> tiles;

      if (vec.size() == 1) {
        // Capture all tiles across all graphs
        auto graphs = xrt_core::edge::aie::get_graphs(device.get());

        for (auto& graph : graphs) {
          /*
           * Core module profiling uses all unique core tiles in aie control
           * Memory module profiling uses all unique core + dma tiles in aie control
           */
          auto coreTiles = xrt_core::edge::aie::get_event_tiles(device.get(), graph,
              xrt_core::edge::aie::module_type::core);
          if (!isCore) {
            auto dmaTiles = xrt_core::edge::aie::get_event_tiles(device.get(), graph,
              xrt_core::edge::aie::module_type::dma);
            std::move(dmaTiles.begin(), dmaTiles.end(), back_inserter(coreTiles));
          }
          std::sort(coreTiles.begin(), coreTiles.end(),
            [](tile_type t1, tile_type t2) {
                if (t1.row == t2.row)
                  return t1.col > t2.col;
                else
                  return t1.row > t2.row;
            }
          );
          std::unique_copy(coreTiles.begin(), coreTiles.end(), back_inserter(tiles),
            [](tile_type t1, tile_type t2) {
                return ((t1.col == t2.col) && (t1.row == t2.row));
            }
          );
        }
      }
      else if (vec.size() == 2) {
        std::vector<std::string> tileVec;
        boost::split(tileVec, vec.at(0), boost::is_any_of(","));

        xrt_core::edge::aie::tile_type tile;
        tile.col = std::stoi(tileVec.at(0));
        tile.row = std::stoi(tileVec.at(1));
        tiles.push_back(tile);
      }
      else if (vec.size() == 3) {
        std::vector<std::string> minTileVec;
        boost::split(minTileVec, vec.at(0), boost::is_any_of(","));
        uint32_t minCol = std::stoi(minTileVec.at(0));
        uint32_t minRow = std::stoi(minTileVec.at(1));
        
        std::vector<std::string> maxTileVec;
        boost::split(maxTileVec, vec.at(1), boost::is_any_of(","));
        uint32_t maxCol = std::stoi(maxTileVec.at(0));
        uint32_t maxRow = std::stoi(maxTileVec.at(1));

        for (uint32_t col = minCol; col <= maxCol; ++col) {
          for (uint32_t row = minRow; row <= maxRow; ++row) {
            xrt_core::edge::aie::tile_type tile;
            tile.col = col;
            tile.row = row;
            tiles.push_back(tile);
          }
        }
      }

      // Report tiles (debug only)
      {
        std::stringstream msg;
        msg << "Tiles used for AIE " << moduleName << " profile counters: ";
        for (auto& tile : tiles) {
          msg << "(" << tile.col << "," << tile.row << "), ";
        }
        xrt_core::message::send(severity_level::debug, "XRT", msg.str());
      }

      // Get vector of pre-defined metrics for this set
      auto startEvents = isCore ? mCoreStartEvents[metricSet] : mMemoryStartEvents[metricSet];
      auto endEvents   = isCore ?   mCoreEndEvents[metricSet] :   mMemoryEndEvents[metricSet];

      uint8_t resetEvent = 0;
      int counterId = 0;
      
      int NUM_COUNTERS = isCore ? NUM_CORE_COUNTERS : NUM_MEMORY_COUNTERS;
      int numTileCounters[NUM_COUNTERS+1] = {0};

      // Ask Resource manager for resource availability
      auto numFreeCounters  = getNumFreeCtr(aieDevice, tiles, isCore, metricSet);

      // Iterate over tiles and metrics to configure all desired counters
      for (auto& tile : tiles) {
        int numCounters = 0;
        auto col = tile.col;
        auto row = tile.row;
        auto mod = isCore ? XAIE_CORE_MOD : XAIE_MEM_MOD;
        auto loc = XAie_TileLoc(col, row + 1);
        // NOTE: resource manager requires absolute row number
        auto& core   = aieDevice->tile(col, row + 1).core();
        auto& memory = aieDevice->tile(col, row + 1).mem();

        for (int i=0; i < numFreeCounters; ++i) {
          // Request counter from resource manager
          auto perfCounter = isCore ? core.perfCounter() : memory.perfCounter();
          auto ret = perfCounter->initialize(mod, startEvents.at(i), mod, endEvents.at(i));
          if (ret != XAIE_OK) break;
          ret = perfCounter->reserve();
          if (ret != XAIE_OK) break;

          // Set masks for group events
          // NOTE: Writing to group error enable register is blocked, so ignoring
          if (startEvents.at(i) == XAIE_EVENT_GROUP_DMA_ACTIVITY_MEM)
            XAie_EventGroupControl(aieDevInst, loc, mod, startEvents.at(i), GROUP_DMA_MASK);
          else if (startEvents.at(i) == XAIE_EVENT_GROUP_LOCK_MEM)
            XAie_EventGroupControl(aieDevInst, loc, mod, startEvents.at(i), GROUP_LOCK_MASK);
          else if (startEvents.at(i) == XAIE_EVENT_GROUP_MEMORY_CONFLICT_MEM)
            XAie_EventGroupControl(aieDevInst, loc, mod, startEvents.at(i), GROUP_CONFLICT_MASK);
          else if (startEvents.at(i) == XAIE_EVENT_GROUP_STREAM_SWITCH_CORE) {
            if (metricSet == "stream_switch_idle")
              XAie_EventGroupControl(aieDevInst, loc, mod, startEvents.at(i), GROUP_STREAM_SWITCH_IDLE_MASK);
            else if (metricSet == "stream_switch_running")
              XAie_EventGroupControl(aieDevInst, loc, mod, startEvents.at(i), GROUP_STREAM_SWITCH_RUNNING_MASK);
            else if (metricSet == "stream_switch_stalled")
              XAie_EventGroupControl(aieDevInst, loc, mod, startEvents.at(i), GROUP_STREAM_SWITCH_STALLED_MASK);
            else if (metricSet == "stream_switch_tlast")
              XAie_EventGroupControl(aieDevInst, loc, mod, startEvents.at(i), GROUP_STREAM_SWITCH_TLAST_MASK);
          } else if (startEvents.at(i) == XAIE_EVENT_GROUP_CORE_PROGRAM_FLOW_CORE) {
            XAie_EventGroupControl(aieDevInst, loc, mod, startEvents.at(i), GROUP_CORE_PROGRAM_FLOW_MASK);
          } else if (startEvents.at(i) == XAIE_EVENT_GROUP_CORE_STALL_CORE) {
            XAie_EventGroupControl(aieDevInst, loc, mod, startEvents.at(i), GROUP_CORE_STALL_MASK);
          }

          // Start the counters after group events have been configured
          ret = perfCounter->start();
          if (ret != XAIE_OK) break;
          mPerfCounters.push_back(perfCounter);

          // Convert enums to physical event IDs for reporting purposes
          int counterNum = i;
          uint8_t phyStartEvent = 0;
          uint8_t phyEndEvent = 0;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, startEvents.at(i), &phyStartEvent);
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, endEvents.at(i), &phyEndEvent);
          if (!isCore) {
            phyStartEvent += BASE_MEMORY_COUNTER;
            phyEndEvent += BASE_MEMORY_COUNTER;
          }

          // Store counter info in database
          std::string counterName = "AIE Counter " + std::to_string(counterId);
          (db->getStaticInfo()).addAIECounter(deviceId, counterId, col, row, counterNum, 
              phyStartEvent, phyEndEvent, resetEvent, clockFreqMhz, moduleName, counterName);
          counterId++;
          numCounters++;
        }

        std::stringstream msg;
        msg << "Reserved " << numCounters << " counters for profiling AIE tile (" << col << "," << row << ").";
        xrt_core::message::send(severity_level::debug, "XRT", msg.str());
        numTileCounters[numCounters]++;
      }

      // Report counters reserved per tile
      {
        std::stringstream msg;
        msg << "AIE profile counters reserved in " << moduleName << " modules - ";
        for (int n=0; n <= NUM_COUNTERS; ++n) {
          if (numTileCounters[n] == 0) continue;
          msg << n << ": " << numTileCounters[n] << " tiles";
          if (n != NUM_COUNTERS) msg << ", ";

          (db->getStaticInfo()).addAIECounterResources(deviceId, n, numTileCounters[n], isCore);
        }
        xrt_core::message::send(severity_level::info, "XRT", msg.str());
      }

      runtimeCounters = true;
    } // for module

    return runtimeCounters;
  }
  
  void AIEProfilingPlugin::pollAIECounters(uint32_t index, void* handle)
  {
    auto it = mThreadCtrlMap.find(handle);
    if (it == mThreadCtrlMap.end())
      return;

    auto& should_continue = it->second;
    while (should_continue) {
      // Wait until xclbin has been loaded and device has been updated in database
      if (!(db->getStaticInfo().isDeviceReady(index)))
        continue;
      XAie_DevInst* aieDevInst =
        static_cast<XAie_DevInst*>(db->getStaticInfo().getAieDevInst(fetchAieDevInst, handle)) ;
      if (!aieDevInst)
        continue;

      uint32_t prevColumn = 0;
      uint32_t prevRow = 0;
      uint64_t timerValue = 0;

      // Iterate over all AIE Counters & Timers
      auto numCounters = db->getStaticInfo().getNumAIECounter(index);
      for (uint64_t c=0; c < numCounters; c++) {
        auto aie = db->getStaticInfo().getAIECounter(index, c);
        if (!aie)
          continue;

        std::vector<uint64_t> values;
        values.push_back(aie->column);
        values.push_back(aie->row);
        values.push_back(aie->startEvent);
        values.push_back(aie->endEvent);
        values.push_back(aie->resetEvent);

        // Read counter value from device
        uint32_t counterValue;
        if (mPerfCounters.empty()) {
          // Compiler-defined counters
          XAie_LocType tileLocation = XAie_TileLoc(aie->column, aie->row + 1);
          XAie_PerfCounterGet(aieDevInst, tileLocation, XAIE_CORE_MOD, aie->counterNumber, &counterValue);
        }
        else {
          // Runtime-defined counters
          auto perfCounter = mPerfCounters.at(c);
          perfCounter->readResult(counterValue);
        }
        values.push_back(counterValue);

        // Read tile timer (once per tile to minimize overhead)
        if ((aie->column != prevColumn) || (aie->row != prevRow)) {
          prevColumn = aie->column;
          prevRow = aie->row;
          XAie_LocType tileLocation = XAie_TileLoc(aie->column, aie->row + 1);
          XAie_ReadTimer(aieDevInst, tileLocation, XAIE_CORE_MOD, &timerValue);
        }
        values.push_back(timerValue);

        // Get timestamp in milliseconds
        double timestamp = xrt_core::time_ns() / 1.0e6;
        db->getDynamicInfo().addAIESample(index, timestamp, values);
      }

      std::this_thread::sleep_for(std::chrono::microseconds(mPollingInterval));     
    }
  }

  void AIEProfilingPlugin::updateAIEDevice(void* handle)
  {
    // Don't update if no profiling is requested
    if (!xrt_core::config::get_aie_profile())
      return;

    char pathBuf[512];
    memset(pathBuf, 0, 512);
    xclGetDebugIPlayoutPath(handle, pathBuf, 512);

    std::string sysfspath(pathBuf);
    uint64_t deviceId = db->addDevice(sysfspath); // Get the unique device Id

    if (!(db->getStaticInfo()).isDeviceReady(deviceId)) {
      // Update the static database with information from xclbin
      (db->getStaticInfo()).updateDevice(deviceId, handle);
      {
        struct xclDeviceInfo2 info;
        if(xclGetDeviceInfo2(handle, &info) == 0) {
          (db->getStaticInfo()).setDeviceName(deviceId, std::string(info.mName));
        }
      }
    }

    // Ensure we only read/configure once per xclbin
    if (!(db->getStaticInfo()).isAIECounterRead(deviceId)) {
      // Update the AIE specific portion of the device
      // When new xclbin is loaded, the xclbin specific datastructure is already recreated

      // 1. Runtime-defined counters
      // NOTE: these take precedence
      bool runtimeCounters = setMetrics(deviceId, handle);

      // 2. Compiler-defined counters
      if (!runtimeCounters) {
        std::shared_ptr<xrt_core::device> device = xrt_core::get_userpf_device(handle);
        auto counters = xrt_core::edge::aie::get_profile_counters(device.get());

        if (counters.empty()) {
          std::string msg = "AIE Profile Counters were not found for this design. "
                            "Please specify aie_profile_core_metrics and/or aie_profile_memory_metrics in your xrt.ini.";
          xrt_core::message::send(severity_level::warning, "XRT", msg);
        }
        else {
          for (auto& counter : counters) {
            (db->getStaticInfo()).addAIECounter(deviceId, counter.id, counter.column,
                counter.row + 1, counter.counterNumber, counter.startEvent, counter.endEvent,
                counter.resetEvent, counter.clockFreqMhz, counter.module, counter.name);
          }
        }
      }

      (db->getStaticInfo()).setIsAIECounterRead(deviceId, true);
    }

    // Open the writer for this device
    struct xclDeviceInfo2 info;
    xclGetDeviceInfo2(handle, &info);
    std::string deviceName = std::string(info.mName);
    // Create and register writer and file
    std::string outputFile = "aie_profile_" + deviceName + ".csv";

    VPWriter* writer = new AIEProfilingWriter(outputFile.c_str(),
                                              deviceName.c_str(), mIndex);
    writers.push_back(writer);
    db->getStaticInfo().addOpenedFile(writer->getcurrentFileName(), "AIE_PROFILE");

    // Start the AIE profiling thread
    mThreadCtrlMap[handle] = true;
    auto device_thread = std::thread(&AIEProfilingPlugin::pollAIECounters, this, mIndex, handle);
    mThreadMap[handle] = std::move(device_thread);

    ++mIndex;
  }

  void AIEProfilingPlugin::endPollforDevice(void* handle)
  {
    // Ask thread to stop
    mThreadCtrlMap[handle] = false;

    auto it = mThreadMap.find(handle);
    if (it != mThreadMap.end()) {
      it->second.join();
      mThreadMap.erase(it);
      mThreadCtrlMap.erase(handle);
    }
  }

  void AIEProfilingPlugin::endPoll()
  {
    // Ask all threads to end
    for (auto& p : mThreadCtrlMap)
      p.second = false;

    for (auto& t : mThreadMap)
      t.second.join();

    mThreadCtrlMap.clear();
    mThreadMap.clear();
  }

} // end namespace xdp
