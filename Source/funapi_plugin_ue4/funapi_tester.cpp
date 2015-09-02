// Copyright (C) 2013-2015 iFunFactory Inc. All Rights Reserved.
//
// This work is confidential and proprietary to iFunFactory Inc. and
// must not be used, disclosed, copied, or distributed without the prior
// consent of iFunFactory Inc.

#include "funapi_plugin_ue4.h"

#if PLATFORM_WINDOWS
#include "AllowWindowsPlatformTypes.h"
#endif

#include <functional>

#include "funapi_tester.h"
#include "funapi/funapi_network.h"
#include "funapi/funapi_downloader.h"
#include "funapi/test_messages.pb.h"

#include "Funapi/FunapiManager.h"
#include "Funapi/ConnectList.h"

// FOR TEST
//#include <WinSock2.h>
//#include <WS2tcpip.h>
//#include "Funapi/JsonAccessor.h"
#include "Funapi/FunapiDownloader.h"
// FOR TEST

#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#if PLATFORM_WINDOWS
#include "HideWindowsPlatformTypes.h"
#endif


namespace
{
    bool is_downloading = false;
    Fun::FunapiDownloader* downloader;

    void on_download_update (const std::string &path, long receive, long total, int percent, void *ctxt)
    {
    }

    void on_download_finished (int result, void *ctxt)
    {
        is_downloading = false;
    }
}


// Sets default values
Afunapi_tester::Afunapi_tester()
{
    // Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
    PrimaryActorTick.bCanEverTick = true;

    // TEST
    //Fun::ConnectList connect;
    //connect.Add("www.naver.com", 8080);
    //while (connect.IsNextAvailable()) {
    //    LOG1("ip: %s", *FString(connect.GetNextAddress()->host.c_str()));
    //}

    //Fun::JsonAccessor<FJsonObject> helper;
    //FString str_json = "{\"sample\":\"hello~\"}";
    //TSharedPtr<FJsonObject> json = helper.Deserialize(str_json);
    //json->SetStringField("item", "new one");
    //json->SetNumberField("num", 321);
    //FString temp = helper.Serialize(json);
    //LOG1("temp: %s", *temp);
    //LOG1("item: %s", *json->GetStringField("item"));
}

Afunapi_tester::~Afunapi_tester()
{
}

// Called when the game starts or when spawned
void Afunapi_tester::BeginPlay()
{
    Super::BeginPlay();

    fun::FunapiNetwork::Initialize();

    // FOR TEST ////////////////////////////////////////////////////
    // downloader = new Fun::FunapiDownloader();
}

void Afunapi_tester::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
  Super::EndPlay(EndPlayReason);

  fun::FunapiNetwork::Finalize();

  if (network) {
    delete network;
    network = nullptr;
  }

    // FOR TEST ////////////////////////////////////////////////////
    // delete downloader;
}

void Afunapi_tester::Tick(float DeltaTime)
{
  Super::Tick(DeltaTime);

  // LOG1("Tick - %f", DeltaTime);

  if (network)
  {
    network->Update();
  }
}

bool Afunapi_tester::ConnectTcp()
{
  Connect(fun::TransportProtocol::kTcp);
  return true;
}

bool Afunapi_tester::ConnectUdp()
{
  Connect(fun::TransportProtocol::kUdp);
  return true;
}

bool Afunapi_tester::ConnectHttp()
{
  Connect(fun::TransportProtocol::kHttp);
  return true;
}

bool Afunapi_tester::IsConnected()
{
    return network != nullptr && network->Connected();
}

void Afunapi_tester::Disconnect()
{
    if (network == nullptr || network->Started() == false)
    {
        LOG("You should connect first.");
        return;
    }

    network->Stop();
}

bool Afunapi_tester::SendEchoMessage()
{
    if (network == NULL || network->Started() == false)
    {
        LOG("You should connect first.");
        return false;
    }

    if (msg_type == fun::kJsonEncoding)
    {
        fun::Json msg;
        msg.SetObject();
        rapidjson::Value message_node("hello world", msg.GetAllocator());
        msg.AddMember("message", message_node, msg.GetAllocator());
        network->SendMessage("echo", msg);
        // network->SendMessage("echo", msg, fun::TransportProtocol::kTcp);
        // network->SendMessage("echo", msg, fun::TransportProtocol::kUdp);
        // network->SendMessage("echo", msg, fun::TransportProtocol::kHttp);
        return true;
    }
    else if (msg_type == fun::kProtobufEncoding)
    {
        FunMessage msg;
        msg.set_msgtype("pbuf_echo");
        PbufEchoMessage *echo = msg.MutableExtension(pbuf_echo);
        echo->set_msg("hello proto");
        network->SendMessage(msg);
        return true;
    }

    return false;
}

void Afunapi_tester::Connect(const fun::TransportProtocol protocol)
{
  if (!network) {
    fun::FunapiTransport *transport = GetNewTransport(protocol);

    network = new fun::FunapiNetwork(transport, msg_type,
      std::bind(&Afunapi_tester::OnSessionInitiated, this, std::placeholders::_1),
      std::bind(&Afunapi_tester::OnSessionClosed, this));

    if (msg_type == fun::kJsonEncoding)
      network->RegisterHandler("echo", std::bind(&Afunapi_tester::OnEchoJson, this, std::placeholders::_1, std::placeholders::_2));
    else if (msg_type == fun::kProtobufEncoding)
      network->RegisterHandler("pbuf_echo", std::bind(&Afunapi_tester::OnEchoProto, this, std::placeholders::_1, std::placeholders::_2));

    network->Start();
  }
  else {
    fun::FunapiTransport *transport = GetNewTransport(protocol);
    network->AttachTransport(transport);
    network->Start();
  }
}

fun::FunapiTransport* Afunapi_tester::GetNewTransport(fun::TransportProtocol protocol)
{
  fun::FunapiTransport *transport = nullptr;

  if (protocol == fun::TransportProtocol::kTcp)
    transport = new fun::FunapiTcpTransport(kServerIp, (uint16_t)(msg_type == fun::kProtobufEncoding ? 8022 : 8012));
  else if (protocol == fun::TransportProtocol::kUdp)
    transport = new fun::FunapiUdpTransport(kServerIp, (uint16_t)(msg_type == fun::kProtobufEncoding ? 8023 : 8013));
  else if (protocol == fun::TransportProtocol::kHttp)
    transport = new fun::FunapiHttpTransport(kServerIp, (uint16_t)(msg_type == fun::kProtobufEncoding ? 8028 : 8018), false);

  return transport;
}

void Afunapi_tester::OnSessionInitiated(const std::string &session_id)
{
  LOG1("session initiated: %s", *FString(session_id.c_str()));
}

void Afunapi_tester::OnSessionClosed()
{
  LOG("session closed");
}

void Afunapi_tester::OnEchoJson(const std::string &type, const std::vector<uint8_t> &v_body)
{
  std::string body(v_body.begin(), v_body.end());

  LOG1("msg '%s' arrived.", *FString(type.c_str()));
  LOG1("json: %s", *FString(body.c_str()));
}

void Afunapi_tester::OnEchoProto(const std::string &type, const std::vector<uint8_t> &v_body)
{
  LOG1("msg '%s' arrived.", *FString(type.c_str()));

  std::string body(v_body.begin(), v_body.end());

  FunMessage msg;
  msg.ParseFromString(body);
  PbufEchoMessage echo = msg.GetExtension(pbuf_echo);
  LOG1("proto: %s", *FString(echo.msg().c_str()));
}

bool Afunapi_tester::FileDownload()
{
    fun::FunapiHttpDownloader downloader(
#if PLATFORM_WINDOWS
        "C:\\Users\\Public\\resource_test",
#else
        "/Users/Shared/resource_test",
#endif
        fun::FunapiHttpDownloader::OnUpdate(on_download_update, this),
        fun::FunapiHttpDownloader::OnFinished(on_download_finished, this));

    is_downloading = true;
    downloader.StartDownload(kServerIp.c_str(), 8020, "list");

    while (is_downloading) {
#if PLATFORM_WINDOWS
      Sleep(1000);
#else
      sleep(1);
#endif
    }

    return true;
}