#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <iomanip>

#include "ns3/core-module.h"
#include "ns3/mobility-module.h"
#include "ns3/leo-module.h"
#include "ns3/network-module.h"
#include "ns3/aodv-module.h"
#include "ns3/udp-server.h"
#include "ns3/point-to-point-module.h"

using namespace ns3;

int port = 9;
NodeContainer satellites;
NodeContainer groundStations;
NetDeviceContainer utNet;
ApplicationContainer sinkApps;

std::map<std::pair<int, int>, double> datarateMap;
std::vector<std::pair<int, int>> gsToSatPairs;
std::map<int, double> satCollectionTime;
std::map<int, bool> gsHasStarted;
std::map<int, double> gsTxStartTime;
std::map<int, int> satReceivingFromGs;
std::map<int, bool> satIsIdle;
Ipv4InterfaceContainer satInterfaces;
std::map<int, double> gsRxEndTime;
std::map<int, double> satFinishTime;
std::map<int, std::vector<int>> satAssignedGs;
std::map<int, size_t> satNextGsIndex;
std::map<int, uint64_t> satLastRxBytes;
std::map<int, int> satTaskProgress;
std::map<int, double> satTimeAccumulated;

// Lab4 specific global variables
int numGroundStations = 0, numSatellites = 0, numLinks = 0;
std::vector<std::pair<int, int>> g_gsToSatAssignments; // List of {gsId, satId} tasks
double g_lab3TotalCollectionTime = 0.0; // Max transmission time from Lab3's output file
std::map<int, std::vector<int>> g_satelliteTaskQueue; // Map of satId -> list of gsIds assigned to it
std::map<int, int> g_satelliteCurrentTaskIndex; // Current task index for each satellite
std::map<int, bool> g_isSatelliteIdle; // Satellite busy status
std::map<int, double> g_lab3SatelliteCollectionTimes; // Individual satellite collection times from Lab3
std::map<int, double> g_finalSatelliteCollectionTime; // Final collection time for each satellite
std::map<int, double> g_gsTransmissionStartTime; // Transmission start time for each GS
std::map<int, double> g_gsTransmissionEndTime; // Transmission end time for each GS

// Forward declarations
static void EchoRx(std::string context, const Ptr<const Packet> packet, const TcpHeader &header, const Ptr<const TcpSocketBase> socket);
void SendPacket(int gsId, int satId);
std::string GetNodeId(std::string str);
void Connect();
void StartGsToSatTransmission(int gsId, int satId);

// Implementation of missing StartGsToSatTransmission function
void StartGsToSatTransmission(int gsId, int satId) {
  g_isSatelliteIdle[satId] = false; // Mark satellite as busy
  SendPacket(gsId, satId);
}

static void EchoRx(std::string context, const Ptr<const Packet> packet, const TcpHeader &header, const Ptr<const TcpSocketBase> socket) {
  uint32_t nodeId = std::stoi(GetNodeId(context));
  if (nodeId >= satellites.GetN()) return;
  int satId = nodeId;

  if (satReceivingFromGs.find(satId) == satReceivingFromGs.end()) return;
  int gsId = satReceivingFromGs[satId];

  Ptr<PacketSink> sink = sinkApps.Get(satId)->GetObject<PacketSink>();
  uint64_t totalRx = sink->GetTotalRx();
  uint64_t delta = totalRx - satLastRxBytes[satId];

  if (delta >= 125000) {
    satLastRxBytes[satId] = totalRx;

    double now = Simulator::Now().GetSeconds();
    gsRxEndTime[gsId] = now;
    g_gsTransmissionEndTime[gsId] = now; // Update global tracking map

    if (gsTxStartTime.find(gsId) != gsTxStartTime.end()) {
      double duration = now - gsTxStartTime[gsId];
      satTimeAccumulated[satId] += duration;
    }

    satTaskProgress[satId]++;
    if ((int)satAssignedGs[satId].size() > satTaskProgress[satId]) {
      int nextGs = satAssignedGs[satId][satTaskProgress[satId]];
      Simulator::ScheduleNow(&SendPacket, nextGs, satId);
    } else {
      satFinishTime[satId] = satTimeAccumulated[satId];
      g_finalSatelliteCollectionTime[satId] = satTimeAccumulated[satId]; // Update global tracking map
      std::cout << "[INFO] SAT " << satId << " finished all tasks, total time: "
                << std::fixed << std::setprecision(6) << satFinishTime[satId] << "s\n";
    }
  }
}

void SendPacket(int gsId, int satId) {
  Ptr<Node> gsNode = groundStations.Get(gsId);
  Ptr<Node> satNode = satellites.Get(satId);

  auto it = datarateMap.find({gsId, satId});
  if (it == datarateMap.end()) {
      std::cerr << "[ERROR] No link found between GS " << gsId << " and SAT " << satId << std::endl;
      return;
  }

  double rate = it->second;
  std::string rateStr = std::to_string(rate) + "kbps";

  Ptr<NetDevice> gsDev = utNet.Get(gsNode->GetId());
  Ptr<NetDevice> satDev = utNet.Get(satNode->GetId());

  gsDev->GetObject<MockNetDevice>()->SetDataRate(rateStr);
  satDev->GetObject<MockNetDevice>()->SetDataRate(rateStr);

  Ptr<Ipv4> ipv4 = satNode->GetObject<Ipv4>();
  Ipv4Address remoteIp = ipv4->GetAddress(1, 0).GetLocal();

  Address remoteAddress(InetSocketAddress(remoteIp, port));
  BulkSendHelper bulkSender("ns3::TcpSocketFactory", remoteAddress);
  bulkSender.SetAttribute("MaxBytes", UintegerValue(125000));
  bulkSender.SetAttribute("SendSize", UintegerValue(512));

  ApplicationContainer senderApp = bulkSender.Install(gsNode);
  senderApp.Start(Seconds(0.0));

  gsHasStarted[gsId] = true;
  gsTxStartTime[gsId] = Simulator::Now().GetSeconds();
  g_gsTransmissionStartTime[gsId] = Simulator::Now().GetSeconds(); // Update global tracking map
  satReceivingFromGs[satId] = gsId;
  satIsIdle[satId] = false;

  std::cout << "[INFO] GS " << gsId << " -> SAT " << satId
            << " started sending @ " << Simulator::Now().GetSeconds() << "s" << std::endl;
}

std::string GetNodeId(std::string str) {
  size_t pos1 = str.find("/", 0);
  size_t pos2 = str.find("/", pos1 + 1);
  size_t pos3 = str.find("/", pos2 + 1);
  return str.substr(pos2 + 1, pos3 - pos2 - 1);
}

void Connect(){
  Config::Connect("/NodeList/*/$ns3::TcpL4Protocol/SocketList/*/Rx", MakeCallback(&EchoRx));
}

NS_LOG_COMPONENT_DEFINE("Lab4");

int main(int argc, char *argv[]) {
  CommandLine cmd;
  std::string constellation = "TelesatGateway";
  double duration = 100.0;
  std::string inputFile = "network.greedy.out";
  std::string outputFile = "lab4.greedy.out";

  cmd.AddValue("duration", "Duration of the simulation in seconds", duration);
  cmd.AddValue("constellation", "LEO constellation link settings name", constellation);
  cmd.AddValue("inputFile", "Input file", inputFile);
  cmd.AddValue("outputFile", "Output file", outputFile);
  cmd.Parse(argc, argv);

  // Default setting
  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(512));
  Config::SetDefault("ns3::TcpSocketBase::MinRto", TimeValue(Seconds(2.0)));
  
  // Satellite
  LeoOrbitNodeHelper orbit;
  satellites = orbit.Install({ LeoOrbit(1200, 20, 1, 60)});

  // Ground station
  LeoGndNodeHelper ground;
  ground.Add(groundStations, LeoLatLong(20, 4));
  ground.Add(groundStations, LeoLatLong(19, 12));
  ground.Add(groundStations, LeoLatLong(19, 10));
  ground.Add(groundStations, LeoLatLong(19, 19));
  ground.Add(groundStations, LeoLatLong(19, 20));
  ground.Add(groundStations, LeoLatLong(18, 20));
  ground.Add(groundStations, LeoLatLong(18, 22));
  ground.Add(groundStations, LeoLatLong(17, 26));
  ground.Add(groundStations, LeoLatLong(18, 30));
  ground.Add(groundStations, LeoLatLong(15, 40));
  ground.Add(groundStations, LeoLatLong(14, 25));
  ground.Add(groundStations, LeoLatLong(14, 30));
  ground.Add(groundStations, LeoLatLong(14, 40));
  ground.Add(groundStations, LeoLatLong(14, 50));
  ground.Add(groundStations, LeoLatLong(14, 52));
  ground.Add(groundStations, LeoLatLong(13, 50));
  ground.Add(groundStations, LeoLatLong(13, 48));
  ground.Add(groundStations, LeoLatLong(12, 50));
  ground.Add(groundStations, LeoLatLong(13, 52));
  ground.Add(groundStations, LeoLatLong(15, 30));

  // Set network
  LeoChannelHelper utCh;
  utCh.SetConstellation(constellation);
  utNet = utCh.Install(satellites, groundStations);

  AodvHelper aodv;
  aodv.Set("EnableHello", BooleanValue(false));
  
  InternetStackHelper stack;
  stack.SetRoutingHelper(aodv);
  stack.Install(satellites);
  stack.Install(groundStations);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.1.0.0", "255.255.0.0");
  ipv4.Assign(utNet);

  // Receiver: satellites
  PacketSinkHelper sink("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
  for(int i=0; i<60; i++){
    sinkApps.Add(sink.Install(satellites.Get(i)));
  }

  // Task 1: Input File - Read network graph
  std::ifstream graphFile("network.graph");
  if (!graphFile.is_open()) {
    NS_LOG_ERROR("Cannot open input file network.graph");
    return 1;
  }
  std::string line;
  bool isFirstGraphLine = true;
  while (std::getline(graphFile, line)) {
    if (line.empty()) continue;
    if (isFirstGraphLine) {
      std::istringstream headerStream(line);
      headerStream >> numGroundStations >> numSatellites >> numLinks;
      isFirstGraphLine = false;
      continue;
    }
    std::istringstream lineStream(line);
    int gsId, satId;
    double dataRate;
    if (lineStream >> gsId >> satId >> dataRate) {
      datarateMap[{gsId, satId}] = dataRate;
    }
  }
  graphFile.close();

  // Read input assignments file
  std::ifstream associationFile(inputFile);
  if (!associationFile.is_open()) {
    NS_LOG_ERROR("Cannot open input file: " << inputFile);
    return 1;
  }

  bool readLab3TotalTime = false;
  int connectionsReadCount = 0;
  while (std::getline(associationFile, line)) {
    if (line.empty()) continue;
    std::istringstream lineStream(line);
    if (!readLab3TotalTime) {
      lineStream >> g_lab3TotalCollectionTime; // Lab3's overall optimal time
      readLab3TotalTime = true;
      continue;
    }
    // Assuming the input file has numGroundStations lines for GS-SAT assignments after the first line
    if (connectionsReadCount < numGroundStations) {
      int gsId, satId;
      if (lineStream >> gsId >> satId) {
        g_gsToSatAssignments.emplace_back(gsId, satId);
        satAssignedGs[satId].push_back(gsId); // Add to both global and local tracking
        connectionsReadCount++;
        continue; // Continue to next line after reading a GS-SAT pair
      }
    }
    // After GS-SAT assignments, the rest are Lab3's satellite-specific collection times
    int satId;
    double lab3SatCollectionTime;
    if (lineStream >> satId >> lab3SatCollectionTime) {
      g_lab3SatelliteCollectionTimes[satId] = lab3SatCollectionTime;
    }
  }
  associationFile.close();

  // Build satellite task queues from assignments
  for (const auto& assignment : g_gsToSatAssignments) {
    g_satelliteTaskQueue[assignment.second].push_back(assignment.first);
  }

  // Initialize satellite task progress tracking
  for (int i = 0; i < numSatellites; i++) {
    satLastRxBytes[i] = 0;
    satTaskProgress[i] = 0;
    satTimeAccumulated[i] = 0.0;
    g_isSatelliteIdle[i] = true; // All satellites start idle
    g_satelliteCurrentTaskIndex[i] = 0;
  }

  // Task 2.2: Start initial transmissions for each satellite
  for (auto const& [satId, gsList] : g_satelliteTaskQueue) {
    if (!gsList.empty() && g_isSatelliteIdle[satId]) { // Check if satellite is idle
      int firstGsId = gsList[0]; // Start with first ground station in the queue
      StartGsToSatTransmission(firstGsId, satId); // This function will mark satellite as busy
    }
  }

  Simulator::Schedule(Seconds(1e-7), &Connect);
  Simulator::Stop(Seconds(duration));
  Simulator::Run();
  Simulator::Destroy();

  // Task 4: Output File
  std::ofstream outputFileStream(outputFile);
  if (!outputFileStream.is_open()) {
    NS_LOG_ERROR("Cannot open output file: " << outputFile);
    return 1;
  }

  double overallMaxCollectionDuration = 0.0;
  for (const auto& pair : g_finalSatelliteCollectionTime) {
    overallMaxCollectionDuration = std::max(overallMaxCollectionDuration, pair.second);
  }

  // Line 1: total_collection_time
  outputFileStream << overallMaxCollectionDuration << "\n";
  outputFileStream << "\n"; // Blank line as in user's original output attempt

  // Lines 2+: satellite_id collection_time (accumulated duration)
  // Output for all satellites defined by numSatellites (from network.graph)
  for (int satId = 0; satId < numSatellites; ++satId) {
    double collectionDuration = g_finalSatelliteCollectionTime.count(satId) ? g_finalSatelliteCollectionTime[satId] : 0.0;
    outputFileStream << satId << " ";
    if (collectionDuration == 0.0) { // Specific handling for 0.0 from user's code
      outputFileStream << "0\n";
    } else {
      outputFileStream << collectionDuration << "\n";
    }
  }
  outputFileStream << "\n"; // Blank line

  // Ground station trans_start_time and recept_end_time
  // Output for all ground stations defined by numGroundStations
  for (int gsId = 0; gsId < numGroundStations; ++gsId) {
    double txStartTime = g_gsTransmissionStartTime.count(gsId) ? g_gsTransmissionStartTime[gsId] : 0.0;
    double rxEndTime = g_gsTransmissionEndTime.count(gsId) ? g_gsTransmissionEndTime[gsId] : 0.0;
    outputFileStream << gsId << " ";
    if (txStartTime == 0.0) {
      outputFileStream << "0 ";
    } else {
      outputFileStream << txStartTime << " ";
    }
    if (rxEndTime == 0.0) {
      outputFileStream << "0\n";
    } else {
      outputFileStream << rxEndTime << "\n";
    }
  }

  outputFileStream.close();
  NS_LOG_INFO("Output successfully written to: " << outputFile);

  return 0;
}
