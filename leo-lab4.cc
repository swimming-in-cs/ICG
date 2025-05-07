#include <iostream>
#include <algorithm>
#include <fstream>      // ifstream/ofstream
#include <sstream>      // istringstream
#include <string>       // std::string
#include <map>          // std::map
#include <vector>       // std::vector
#include <iomanip> // for std::setprecision

#include "ns3/core-module.h"
#include "ns3/mobility-module.h"
#include "ns3/leo-module.h"
#include "ns3/network-module.h"
#include "ns3/aodv-module.h"
#include "ns3/udp-server.h"
#include "ns3/point-to-point-module.h"


using namespace ns3;

// Satellite network setup
int port = 9;
NodeContainer satellites;
NodeContainer groundStations;
NetDeviceContainer utNet;
ApplicationContainer sinkApps;


// std::map<std::pair<int, int>, double> linkDataRate;
// std::vector<std::pair<int, int>> connections;
// std::map<int, double> dataCollectionTime;
// std::map<int, double> txStartTime;
// std::map<int, bool> gsStarted;
// std::map<int, bool> satIdle;
// double maxTransmissionTime = 0.0;
// int numGs = 0, numSats = 0, numLinks = 0;

std::map<std::pair<int, int>, double> linkDataRate; // 從 network.graph 載入的 datarate
std::vector<std::pair<int, int>> connections;       // GS -> SAT 任務分配
std::map<int, double> dataCollectionTime;           // 每顆衛星任務耗時

std::map<int, bool> gsStarted;                      // GS 是否已開始傳送
std::map<int, double> groundTxStartTime;            // GS 傳送起始時間
std::map<int, int> currentTransmission;             // SAT 對應目前接收哪一個 GS 的封包
std::map<int, bool> satIdle;                        // (可選) SAT 是否 idle
Ipv4InterfaceContainer satelliteInterfaces;         // 指定 SAT 的 IP 接口

std::map<int, double> groundRxEndTime;             // GS 封包接收完成時間
std::map<int, double> satRxEndTime;                // SAT 完成所有分配傳輸時間
std::map<int, std::vector<int>> associations;      // SAT -> 多個 GS 的關聯表
std::map<int, size_t> associationIndex;


std::map<int, uint64_t> lastTotalRx;                      // 紀錄每顆 SAT 上次接收的總位元組數
std::map<int, int> satProgressIndex;                      // 紀錄 SAT 現在處理到第幾個 GS
std::map<int, double> satAccumulatedTime;                 // 每顆 SAT 的累加傳輸時間


double maxTransmissionTime = 0.0;
int numGs = 0, numSats = 0, numLinks = 0;

static void EchoRx(std::string context, const Ptr< const Packet > packet, const TcpHeader &header, const Ptr< const TcpSocketBase > socket);
void SendPacket(int gsId, int satId);
string GetNodeId(string str);
void Connect();

// static void EchoRx(std::string context, const Ptr< const Packet > packet, const TcpHeader &header, const Ptr< const TcpSocketBase > socket){	
//   // Task 3: Complete this function

static void EchoRx(std::string context, const Ptr<const Packet> packet, const TcpHeader &header, const Ptr<const TcpSocketBase> socket) {
  uint32_t nodeId = std::stoi(GetNodeId(context));
  if (nodeId >= satellites.GetN()) return; // 不是衛星
  int satId = nodeId;

  if (currentTransmission.find(satId) == currentTransmission.end()) return;
  int gsId = currentTransmission[satId];

  Ptr<PacketSink> sink = sinkApps.Get(satId)->GetObject<PacketSink>();
  uint64_t totalRx = sink->GetTotalRx();

  // 判斷這一筆是否剛好完成一筆傳送
  uint64_t delta = totalRx - lastTotalRx[satId];
  if (delta >= 125000) {
    lastTotalRx[satId] = totalRx;

    double now = Simulator::Now().GetSeconds();
    groundRxEndTime[gsId] = now;

    // 統計這筆傳輸時間
    if (groundTxStartTime.find(gsId) != groundTxStartTime.end()) {
      double duration = now - groundTxStartTime[gsId];
      satAccumulatedTime[satId] += duration;
    }

    // 檢查 SAT 是否還有下一個任務
    satProgressIndex[satId]++;
    if ((int)associations[satId].size() > satProgressIndex[satId]) {
      int nextGs = associations[satId][satProgressIndex[satId]];
      Simulator::ScheduleNow(&SendPacket, nextGs, satId);
    } else {
      // 所有任務完成
      satRxEndTime[satId] = satAccumulatedTime[satId];
      std::cout << "[INFO] SAT " << satId << " finished all tasks, total time: "
                << std::fixed << std::setprecision(6) << satRxEndTime[satId] << "s\n";
    }
  }
}



//先看 total rx 因為sat 會有很多gs 因此傳完的時候再多call sendpack

// void SendPacket(int gsId, int satId){
//   // Task 2.1: Complete this function

// }

void SendPacket(int gsId, int satId) {
  Ptr<Node> gsNode = groundStations.Get(gsId);
  Ptr<Node> satNode = satellites.Get(satId);

  auto it = linkDataRate.find({gsId, satId});
  if (it == linkDataRate.end()) {
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
  Ipv4Address remoteIp = ipv4->GetAddress(1, 0).GetLocal();  // interfaceIndex=1 為 UT 介面

  Address remoteAddress(InetSocketAddress(remoteIp, port));
  BulkSendHelper bulkSender("ns3::TcpSocketFactory", remoteAddress);
  bulkSender.SetAttribute("MaxBytes", UintegerValue(125000));
  bulkSender.SetAttribute("SendSize", UintegerValue(512));

  ApplicationContainer senderApp = bulkSender.Install(gsNode);
  senderApp.Start(Seconds(0.0));
  // senderApp.Start(Simulator::Now());
  gsStarted[gsId] = true;
  groundTxStartTime[gsId] = Simulator::Now().GetSeconds();
  currentTransmission[satId] = gsId;
  satIdle[satId] = false;

  std::cout << "[INFO] GS " << gsId << " -> SAT " << satId
            << " started sending @ " << Simulator::Now().GetSeconds() << "s" << std::endl;
}


string GetNodeId(string str) {
  // Which node
  size_t pos1 = str.find("/", 0);           // The first "/"
  size_t pos2 = str.find("/", pos1 + 1);    // The second "/"
  size_t pos3 = str.find("/", pos2 + 1);    // The third "/"
  return str.substr(pos2 + 1, pos3 - pos2 - 1); // Node id
}

void Connect(){
  Config::Connect ("/NodeList/*/$ns3::TcpL4Protocol/SocketList/*/Rx", MakeCallback (&EchoRx));
}

NS_LOG_COMPONENT_DEFINE ("Lab4");

int main(int argc, char *argv[]){

  CommandLine cmd;
  string constellation = "TelesatGateway";
  double duration = 100;
  string inputFile = "network.greedy.out";
  string outputFile = "lab4.greedy.out";

  cout << outputFile << endl;

  cmd.AddValue("duration", "Duration of the simulation in seconds", duration);
  cmd.AddValue("constellation", "LEO constellation link settings name", constellation);
  cmd.AddValue("inputFile", "Input file", inputFile);
  cmd.AddValue("outputFile", "Output file", outputFile);
  cmd.Parse (argc, argv);

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
  utCh.SetConstellation (constellation);
  utNet = utCh.Install (satellites, groundStations);

  AodvHelper aodv;
  aodv.Set ("EnableHello", BooleanValue (false));
  
  InternetStackHelper stack;
  stack.SetRoutingHelper (aodv);
  stack.Install (satellites);
  stack.Install (groundStations);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.1.0.0", "255.255.0.0");
  // ipv4.Assign (utNet);
  satelliteInterfaces = ipv4.Assign(utNet);

  // Receiver: satellites
  PacketSinkHelper sink("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
  for(int i=0; i<60; i++){
    sinkApps.Add(sink.Install(satellites.Get(i)));
  }

  // Task 1: Input File

  // === Read and parse network.graph ===
  std::ifstream graphFile("network.graph");
  if (!graphFile.is_open()) {
      std::cerr << "Failed to open network.graph" << std::endl;
      return 1;
  }

  std::string line;
  bool isFirstLine = true;
  while (std::getline(graphFile, line)) {
      if (line.empty()) continue;

      if (isFirstLine) {
          std::istringstream header(line);
          header >> numGs >> numSats >> numLinks;
          isFirstLine = false;
          continue;
      }

      std::istringstream iss(line);
      int gsId, satId;
      double datarate;
      if (iss >> gsId >> satId >> datarate) {
          linkDataRate[{gsId, satId}] = datarate;
      }
  }
  graphFile.close();

  //確定link 存在
  cout << "test" << endl;

  // === Read and parse network.ortools.out ===
  std::ifstream ortoolsFile(inputFile);
  if (!ortoolsFile.is_open()) {
      std::cerr << "Failed to open " << inputFile << std::endl;
      return 1;
  }

  bool readMaxTime = false;
  int connectionLinesRead = 0;
  while (std::getline(ortoolsFile, line)) {
      if (line.empty()) continue;

      std::istringstream iss(line);
      if (!readMaxTime) {
          iss >> maxTransmissionTime;
          readMaxTime = true;
          continue;
      }

      if (connectionLinesRead < numGs) {
          int gsId, satId;
          if (iss >> gsId >> satId) {
              connections.emplace_back(gsId, satId);
              connectionLinesRead++;
              continue;
          }
      }

      int satId;
      double collectionTime;
      if (iss >> satId >> collectionTime) {
          dataCollectionTime[satId] = collectionTime;
      }
  }
  ortoolsFile.close();

  
    
  std::cout << "Task 1 completed." << std::endl;

  for (const auto& conn : connections) {
    int gsId = conn.first;
    int satId = conn.second;
    associations[satId].push_back(gsId);
  }
  for (const auto& [satId, _] : associations) {
      associationIndex[satId] = 0;  // 每顆 SAT 從 index 0 開始傳
  }
  for (const auto& [satId, gsList] : associations) {
    if (!gsList.empty()) {
      int firstGs = gsList[0];
      gsStarted[firstGs] = false;
      satIdle[satId] = true;
      SendPacket(firstGs, satId);
      satIdle[satId] = false;
    }
  }
  


  Simulator::Schedule(Seconds(1e-7), &Connect);
  Simulator::Stop (Seconds (duration));
  Simulator::Run ();
  Simulator::Destroy ();
  // Task 4: Output File

  std::ofstream outFile(outputFile);
  if (!outFile.is_open()) {
      std::cerr << "[ERROR] Failed to open output file: " << outputFile << std::endl;
      return 1;
  }

  // 計算最大 collection time
  double totalCollectionTime = 0.0;
  for (const auto& kv : satRxEndTime) {
      totalCollectionTime = std::max(totalCollectionTime, kv.second);
  }

  // Line 1: total_collection_time
  outFile << std::fixed << std::setprecision(6) << totalCollectionTime << "\n";
  outFile << "\n";
  // Line 2+: satellite_id collection_time
  for (int satId = 0; satId < numSats; ++satId) {
    double rxTime = satRxEndTime.count(satId) ? satRxEndTime[satId] : 0;
    outFile << satId << " ";
    if (rxTime == 0.0)
        outFile << "0\n";
    else
        outFile << std::fixed << std::setprecision(6) << rxTime << "\n";
  }


  outFile << "\n";
  // Ground station trans_start_time and receive_end_time
  for (int gsId = 0; gsId < numGs; ++gsId) {
    double txTime = groundTxStartTime.count(gsId) ? groundTxStartTime[gsId] : 0;
    double rxTime = groundRxEndTime.count(gsId) ? groundRxEndTime[gsId] : 0;
    outFile << gsId << " ";
    if (txTime == 0.0)
        outFile << "0 ";
    else
        outFile << std::fixed << std::setprecision(6) << txTime << " ";

    if (rxTime == 0.0)
        outFile << "0\n";
    else
        outFile << std::fixed << std::setprecision(6) << rxTime << "\n";
  }


  outFile.close();
  std::cout << "[INFO] Output written to: " << outputFile << std::endl;



  return 0;
}
