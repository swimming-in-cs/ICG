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

// Data rate map: GS -> SAT
std::map<std::pair<int,int>, double> dataRateMap;

// Task queue: satelliteId -> list of GS IDs
std::map<int, std::vector<int>> satelliteTaskQueue;

// Tracking maps
std::map<int, bool> g_isSatelliteIdle;
std::map<int, double> g_gsTxStartTime;
std::map<int, double> g_gsRxEndTime;
std::map<int, double> g_finalSatelliteCollectionTime;

// Per-satellite progress
std::map<int, int> satTaskProgress;
std::map<int, uint64_t> satLastRxBytes;
std::map<int, double> satTimeAccumulated;

static void EchoRx(std::string context, const Ptr<const Packet> packet,
                   const TcpHeader &header, const Ptr<const TcpSocketBase> socket);
void SendPacket(int gsId, int satId);
std::string GetNodeId(const std::string &str);
void Connect();
void StartGsToSatTransmission(int gsId, int satId);

void StartGsToSatTransmission(int gsId, int satId) {
    g_isSatelliteIdle[satId] = false;
    SendPacket(gsId, satId);
}

static void EchoRx(std::string context, const Ptr<const Packet> packet,
                   const TcpHeader &header, const Ptr<const TcpSocketBase> socket) {
    int satId = std::stoi(GetNodeId(context));
    if (satId < 0 || satId >= (int)satellites.GetN()) return;

    // Identify GS sending to this satellite
    auto &queue = satelliteTaskQueue[satId];
    int progress = satTaskProgress[satId];

    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApps.Get(satId));
    uint64_t totalRx = sink->GetTotalRx();
    uint64_t delta = totalRx - satLastRxBytes[satId];

    if (delta >= 125000) {
        satLastRxBytes[satId] = totalRx;
        double now = Simulator::Now().GetSeconds();

        int gsId = queue[progress];
        g_gsRxEndTime[gsId] = now;

        // Accumulate time
        double start = g_gsTxStartTime[gsId];
        satTimeAccumulated[satId] += (now - start);

        satTaskProgress[satId]++;
        if (satTaskProgress[satId] < (int)queue.size()) {
            int nextGs = queue[satTaskProgress[satId]];
            Simulator::ScheduleNow(&SendPacket, nextGs, satId);
        } else {
            g_finalSatelliteCollectionTime[satId] = satTimeAccumulated[satId];
            std::cout << "[INFO] SAT " << satId << " finished all tasks, total time: "
                      << std::fixed << std::setprecision(6)
                      << satTimeAccumulated[satId] << "s\n";
        }
    }
}

void SendPacket(int gsId, int satId) {
    // Compute device indices in utNet: first groundStations, then satellites
    uint32_t gsIndex  = gsId;
    uint32_t satIndex = groundStations.GetN() + satId;

    Ptr<NetDevice> gsDev  = utNet.Get(gsIndex);
    Ptr<NetDevice> satDev = utNet.Get(satIndex);

    auto it = dataRateMap.find({gsId, satId});
    if (it == dataRateMap.end()) {
        std::cerr << "[ERROR] No link for GS " << gsId << " -> SAT " << satId << "\n";
        return;
    }
    double rate = it->second;
    std::string rateStr = std::to_string(rate) + "kbps";

    gsDev->GetObject<MockNetDevice>()->SetDataRate(rateStr);
    satDev->GetObject<MockNetDevice>()->SetDataRate(rateStr);

    Ptr<Node> satNode = satellites.Get(satId);
    Ptr<Ipv4> ipv4    = satNode->GetObject<Ipv4>();
    Ipv4Address remoteIp = ipv4->GetAddress(1,0).GetLocal();

    Address remoteAddress(InetSocketAddress(remoteIp, port));
    BulkSendHelper bulkSender("ns3::TcpSocketFactory", remoteAddress);
    bulkSender.SetAttribute("MaxBytes", UintegerValue(125000));
    bulkSender.SetAttribute("SendSize", UintegerValue(512));

    ApplicationContainer senderApp = bulkSender.Install(groundStations.Get(gsId));
    senderApp.Start(Seconds(0.0));

    g_gsTxStartTime[gsId] = Simulator::Now().GetSeconds();
    g_isSatelliteIdle[satId] = false;

    std::cout << "[INFO] GS " << gsId << " -> SAT " << satId
              << " started at " << Simulator::Now().GetSeconds() << "s\n";
}

std::string GetNodeId(const std::string &str) {
    // context: "/NodeList/X/$ns3::Tcp..."
    size_t p1 = str.find('/', 1);
    size_t p2 = str.find('/', p1+1);
    return str.substr(p1+1, p2-p1-1);
}

void Connect() {
    Config::Connect("/NodeList/*/$ns3::TcpL4Protocol/SocketList/*/Rx",
                    MakeCallback(&EchoRx));
}

NS_LOG_COMPONENT_DEFINE("Lab4");

int main(int argc, char *argv[]) {
    CommandLine cmd;
    double duration = 100.0;
    std::string constellation = "TelesatGateway";
    std::string inputFile  = "network.greedy.out";
    std::string outputFile = "lab4.greedy.out";

    cmd.AddValue("duration", "Simulation duration (s)", duration);
    cmd.AddValue("constellation", "LEO constellation name", constellation);
    cmd.AddValue("inputFile", "GS-SAT assignment file", inputFile);
    cmd.AddValue("outputFile", "Results output file", outputFile);
    cmd.Parse(argc, argv);

    // Defaults
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(512));
    Config::SetDefault("ns3::TcpSocketBase::MinRto", TimeValue(Seconds(2.0)));

    // Install satellites and ground stations
    LeoOrbitNodeHelper orbit;
    satellites = orbit.Install({LeoOrbit(1200,20,1,60)});

    LeoGndNodeHelper ground;
    // Add your GS positions...
    ground.Add(groundStations, LeoLatLong(20,4));
    // ... (其他地面站)
    utNet = LeoChannelHelper(constellation).Install(satellites, groundStations);

    // Install stack & routing
    AodvHelper aodv; aodv.Set("EnableHello", BooleanValue(false));
    InternetStackHelper stack; stack.SetRoutingHelper(aodv);
    stack.Install(satellites); stack.Install(groundStations);

    // IP addressing
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.0.0","255.255.0.0");
    ipv4.Assign(utNet);

    // Sink on satellites
    PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    for (uint32_t i = 0; i < satellites.GetN(); ++i) {
        sinkApps.Add(sinkHelper.Install(satellites.Get(i)));
    }

    // Read network.graph
    std::ifstream graphFile("network.graph");
    int numGS, numSat, numLinks;
    graphFile >> numGS >> numSat >> numLinks;
    int gsId, satId;
    double rate;
    while (graphFile >> gsId >> satId >> rate) {
        dataRateMap[{gsId, satId}] = rate;
    }
    graphFile.close();

    // Read assignments & Lab3 times
    std::ifstream inFile(inputFile);
    double tmp;
    inFile >> tmp; // skip Lab3 overall time
    for (int i = 0; i < numGS; ++i) {
        inFile >> gsId >> satId;
        satelliteTaskQueue[satId].push_back(gsId);
    }
    int sid; double t;
    while (inFile >> sid >> t) {
        // Lab3 per-sat times ignored
    }
    inFile.close();

    // Initialize per-sat data
    for (int i = 0; i < numSat; ++i) {
        g_isSatelliteIdle[i] = true;
        satTaskProgress[i]    = 0;
        satLastRxBytes[i]     = 0;
        satTimeAccumulated[i] = 0.0;
    }

    // Start first tasks
    for (auto &p : satelliteTaskQueue) {
        int s = p.first;
        if (!p.second.empty() && g_isSatelliteIdle[s]) {
            StartGsToSatTransmission(p.second[0], s);
        }
    }

    Simulator::Schedule(Seconds(1e-7), &Connect);
    Simulator::Stop(Seconds(duration));
    Simulator::Run();
    Simulator::Destroy();

    // Write output
    std::ofstream outFile(outputFile);
    double maxTime = 0;
    for (auto &p : g_finalSatelliteCollectionTime) {
        maxTime = std::max(maxTime, p.second);
    }
    outFile << maxTime << "\n\n";
    for (int i = 0; i < numSat; ++i) {
        double ct = g_finalSatelliteCollectionTime[i];
        outFile << i << " " << ct << "\n";
    }
    outFile << "\n";
    for (int i = 0; i < numGS; ++i) {
        double st = g_gsTxStartTime[i];
        double et = g_gsRxEndTime[i];
        outFile << i << " " << st << " " << et << "\n";
    }
    outFile.close();

    NS_LOG_INFO("Output written to " << outputFile);
    return 0;
}
