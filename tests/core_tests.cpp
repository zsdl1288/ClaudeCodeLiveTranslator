#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include "core.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <algorithm>

using namespace cclive;
using json=nlohmann::json;
namespace {
int checks=0,failures=0;
void check(bool condition,const char* name){++checks;if(!condition){++failures;std::cerr<<"FAIL: "<<name<<"\n";}}
template<class F> void rejects(F f,const char* name){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,name);}
json snap(const std::string& text,const std::string& context="chat-a",const std::string& kind="answer",const std::string& key="a"){
    return {{"context",context},{"blocks",json::array({{{"key",key},{"text",text},{"kind",kind}}})}};
}
std::vector<std::string> texts(const std::vector<json>& events){std::vector<std::string> out;for(const auto& e:events)if(e.value("type","")=="text")out.push_back(e.at("text").get<std::string>());return out;}
struct HttpServer {
    SOCKET listener=INVALID_SOCKET;
    std::thread thread;
    std::string received,base;
    std::atomic<int> requests{0};
    explicit HttpServer(std::string body,std::string type="application/json",int status=200,std::string extra=""){
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(listener==INVALID_SOCKET)throw std::runtime_error("socket");
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=0;
        if(bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))||listen(listener,1))throw std::runtime_error("bind");
        int len=sizeof(addr);getsockname(listener,reinterpret_cast<sockaddr*>(&addr),&len);base="http://127.0.0.1:"+std::to_string(ntohs(addr.sin_port));
        auto listening=listener;
        thread=std::thread([this,listening,body=std::move(body),type=std::move(type),status,extra=std::move(extra)](){
            SOCKET client=accept(listening,nullptr,nullptr);if(client==INVALID_SOCKET)return;
            DWORD timeout=5000;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
            char data[4096];std::size_t goal=0;
            for(;;){int n=recv(client,data,sizeof(data),0);if(n<=0)break;received.append(data,n);
                auto h=received.find("\r\n\r\n");if(h!=std::string::npos){if(!goal){goal=h+4;auto marker=received.find("Content-Length:");if(marker!=std::string::npos)goal+=std::stoul(received.substr(marker+15));}if(received.size()>=goal)break;}
                if(received.size()>100000)break;
            }
            ++requests;
            std::string reply="HTTP/1.1 "+std::to_string(status)+" Test\r\nContent-Type: "+type+"\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n"+extra+"\r\n"+body;
            std::size_t offset=0;while(offset<reply.size()){int n=send(client,reply.data()+offset,static_cast<int>((std::min)(std::size_t(97),reply.size()-offset)),0);if(n<=0)break;offset+=n;}
            shutdown(client,SD_BOTH);closesocket(client);
        });
    }
    ~HttpServer(){if(listener!=INVALID_SOCKET){closesocket(listener);listener=INVALID_SOCKET;}if(thread.joinable())thread.join();}
    void wait(){if(thread.joinable())thread.join();}
};
Settings api(const HttpServer& server,const std::string& provider="openai"){Settings s;s.provider=provider;s.model="test-model";s.base_url=server.base;return s;}
std::string http_date_after(int seconds) {
    FILETIME file{};GetSystemTimeAsFileTime(&file);
    ULARGE_INTEGER value{};value.LowPart=file.dwLowDateTime;value.HighPart=file.dwHighDateTime;
    value.QuadPart=static_cast<ULONGLONG>(static_cast<std::int64_t>(value.QuadPart)+static_cast<std::int64_t>(seconds)*10000000);
    file.dwLowDateTime=value.LowPart;file.dwHighDateTime=value.HighPart;
    SYSTEMTIME system{};wchar_t date[WINHTTP_TIME_FORMAT_BUFSIZE]{};
    if(!FileTimeToSystemTime(&file,&system)||!WinHttpTimeFromSystemTime(&system,date))throw std::runtime_error("HTTP date fixture");
    return to_utf8(date);
}
}
int main(){
    WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))return 2;
    try{
        check(to_utf8(to_wide("中文 English 😀"))=="中文 English 😀","Unicode roundtrip");
        rejects([]{to_wide(std::string("\xff"));},"Invalid UTF8 rejected");
        auto folder=std::filesystem::path(L"work")/L"cpp-core-tests";std::filesystem::create_directories(folder);auto config=folder/L"settings.json";
        std::error_code ignored;std::filesystem::remove(config,ignored);
        check(load_settings(config).provider=="google","Missing config defaults");
        Settings s;s.provider="openai";s.model="翻译模型";s.api_key_env="CCLIVE_TEST_KEY";s.show_original=false;s.always_on_top=true;s.font_size=16;
        SetEnvironmentVariableW(L"CCLIVE_TEST_KEY",L"NEVER_SAVE_THIS_KEY");save_settings(config,s);auto loaded=load_settings(config);
        check(loaded.provider==s.provider&&loaded.model==s.model&&loaded.api_key_env==s.api_key_env&&loaded.font_size==16&&!loaded.show_original&&loaded.always_on_top,"Atomic config roundtrip");
        std::ifstream saved(config,std::ios::binary);std::string saved_text((std::istreambuf_iterator<char>(saved)),{});saved.close();
        check(saved_text.find("NEVER_SAVE_THIS_KEY")==std::string::npos,"Config never saves environment secret");
        {std::ofstream out(config);out<<R"({"target":"zh-TW","api_key":"DISCARD_SECRET","future":true})";}
        loaded=load_settings(config);save_settings(config,loaded);std::ifstream clean(config);std::string clean_text((std::istreambuf_iterator<char>(clean)),{});clean.close();
        check(loaded.target=="zh-TW"&&clean_text.find("DISCARD_SECRET")==std::string::npos&&clean_text.find("future")==std::string::npos,"Unknown fields and pasted secrets discarded");
        for(const auto* bad:{"[]","{bad","{\"font_size\":true}","{\"settle_ms\":0}","{\"api_key_env\":\"not-a-name\"}","{\"provider\":\"typo\"}"}){
            {std::ofstream out(config);out<<bad;}rejects([&]{load_settings(config);},"Bad config rejected");
            std::ifstream before(config);std::string actual((std::istreambuf_iterator<char>(before)),{});check(actual==bad,"Bad config not overwritten");
        }
        std::filesystem::remove(config,ignored);
        check(split_sentences("- First item\n- Second item")==std::vector<std::string>({"- First item","- Second item"}),"Bullet lines without punctuation remain separate");
        check(split_sentences("* First item\n+ Second item\n- Third item")==std::vector<std::string>({"* First item","+ Second item","- Third item"}),"Explicit bullet marker variants remain intact");
        check(split_sentences("## Overview\nThis paragraph wraps\nonto the next line.")==std::vector<std::string>({"## Overview","This paragraph wraps onto the next line."}),"Heading and wrapped body retain separate layout units");
        check(split_sentences("# First. Heading.\nBody sentence.")==std::vector<std::string>({"# First. Heading.","Body sentence."}),"Heading punctuation does not split its style marker from text");
        check(split_sentences("1. First step\n2) Second step\n   continuation")==std::vector<std::string>({"1. First step","2) Second step continuation"}),"Ordered list boundaries and soft continuation preserved");
        check(split_sentences("- Parent item\n  - Nested item\n- Next item")==std::vector<std::string>({"- Parent item","  - Nested item","- Next item"}),"Nested bullet indentation preserved for native layout");
        check(split_sentences("A soft\nwrapped sentence. Another line\ncontinues here.")==std::vector<std::string>({"A soft wrapped sentence.","Another line continues here."}),"Ordinary soft wrapping still uses sentence slots");
        check(split_sentences("## Explanation\n```md\n- Do not translate code\n```\n- Actual bullet\n- More prose")==std::vector<std::string>({"## Explanation","- Actual bullet","- More prose"}),"Code fences stay excluded between heading and bullet paragraphs");
        check(split_sentences("### Third heading\r\n- First line\r- Next line")==std::vector<std::string>({"### Third heading","- First line","- Next line"}),"Structured line boundaries accept Windows newline variants");
        {
            Ledger structured(0);
            auto events=structured.observe_at(snap("## Progress\n- Inspect project\n- Run checks"),0);
            check(texts(events)==std::vector<std::string>({"## Progress","- Inspect project","- Run checks"}),"Ledger emits separate heading and list records for text layout");
        }
        check(split_sentences("Dr. Smith uses version 3.14. This is ready.")==std::vector<std::string>({"Dr. Smith uses version 3.14.","This is ready."}),"Abbreviation and decimal sentence slots");
        check(split_sentences("First prose.\n~~~js\nconst secret='x';\n~~~\nSecond prose.\n```py\nprint('y')\n```\nThird prose.")==std::vector<std::string>({"First prose.","Second prose.","Third prose."}),"Multiple fences excluded");
        check(split_sentences("The explanation is ready.\n```python\nprint('Secret')")==std::vector<std::string>({"The explanation is ready."}),"Unclosed streaming fence excluded");
        check(split_sentences("Here is the fix:\n```cpp\nreturn 0;\n```\nIt is ready.")==std::vector<std::string>({"Here is the fix:","It is ready."}),"Prose before and after code survives");
        {
            Ledger l;check(l.observe_at(snap("Done"),0).empty()&&l.flush_at(500).empty(),"Short reply waits for stability");
            auto e=l.flush_at(1000);check(texts(e)==std::vector<std::string>({"Done"})&&e[0]["stable"]==true,"Stable reply has no second debounce");
            check(l.flush_at(2000).empty()&&l.observe_at(snap("Done"),3000).empty(),"Same text repaint deduplicates");
        }
        {
            Ledger l;l.observe_at(snap("The first sentence is ready. The"),0);l.observe_at(snap("The first sentence is ready. The next"),500);
            check(texts(l.observe_at(snap("The first sentence is ready. The next sentence"),1000))==std::vector<std::string>({"The first sentence is ready."}),"Completed sentence matures while tail grows");
            check(texts(l.flush_at(2000))==std::vector<std::string>({"The next sentence"}),"Growing tail matures independently");
        }
        {
            Ledger l;l.observe_at(snap("I can help"),0);auto first=l.flush_at(1000)[0];l.observe_at(snap("I can help with this."),1100);auto second=l.flush_at(2100)[0];
            check(first["key"]==second["key"]&&second["revision"]==2&&second["text"]=="I can help with this.","Tail rewrite keeps key and advances revision");
            l.observe_at(snap("First sentence. Second sentence."),2200);auto two=l.flush_at(3200);l.observe_at(snap("The corrected result."),3300);auto changed=l.flush_at(4300);
            check(texts(changed)==std::vector<std::string>({"The corrected result.",""})&&changed[1]["key"]==two[1]["key"],"Rewrite clears removed sentence slots");
        }
        {
            Ledger l;l.observe_at(snap("A draft sentence."),0);l.observe_at(snap("The final sentence."),500);check(l.flush_at(1000).empty(),"Pending rewrite does not emit old text");
            check(texts(l.flush_at(1500))==std::vector<std::string>({"The final sentence."}),"Final rewrite settles");
            auto reset=l.observe_at(snap("New reply.","chat-b"),1600);check(reset.size()==1&&reset[0]["type"]=="reset"&&reset[0]["epoch"]==1,"Context switch immediately resets");
            auto next=l.flush_at(2600);check(next.size()==1&&next[0]["epoch"]==1&&next[0]["text"]=="New reply.","Context switch cancels old pending content");
        }
        {
            Ledger l;l.observe_at(snap("Old pending reply."),0);l.observe_at({{"context","chat-a"},{"blocks",json::array()}},500);check(l.flush_at(2000).empty(),"Missing block cannot mature offscreen");
            l.observe_at(snap("Old pending reply."),2100);check(l.flush_at(2500).empty()&&texts(l.flush_at(3100))==std::vector<std::string>({"Old pending reply."}),"Reappearing pending block restabilizes");
            l.observe_at({{"context","chat-a"},{"blocks",json::array()}},3200);check(l.observe_at(snap("Old pending reply."),4500).empty(),"Reappearing emitted block deduplicates");
        }
        {
            Ledger l;l.observe_at(snap("全部修改已完成。\n\n12345"),0);check(l.flush_at(1000).empty(),"Chinese and numbers skip blank cards");
            l.observe_at(snap("All done."),1100);auto first=l.flush_at(2100)[0];l.observe_at(snap("已完成。"),2200);auto clear=l.flush_at(3200)[0];check(clear["text"]==""&&clear["key"]==first["key"]&&clear["revision"]==2,"English to Chinese rewrite removes stale translation");
        }
        {
            Ledger l;l.observe_at(snap("An interim result.","chat-a","intermediate"),0);auto first=l.flush_at(1000)[0];l.observe_at(snap("An interim result.","chat-a","answer"),1100);auto final=l.flush_at(2100)[0];
            check(first["key"]==final["key"]&&final["kind"]=="answer"&&final["revision"]==2,"Kind change retains identity");
            json blocks=json::array();for(int n=0;n<600;++n)blocks.push_back({{"key",std::to_string(n)},{"text","Reply number "+std::to_string(n)+"."}});
            Ledger bounded(0);auto many=bounded.observe_at({{"context","a"},{"blocks",blocks}},0);check(many.size()==512&&many[0]["block_key"]=="88"&&many.back()["block_key"]=="599","Ledger retains latest 512 blocks");
            rejects([]{Ledger bad(-1);},"Negative settle rejected");rejects([&]{l.observe_at(json::array(),0);},"Invalid snapshot rejected");
        }
        check(parse_openai_content(json::parse(R"({"choices":[{"delta":{"content":"你"}}]})"),true)=="你","OpenAI delta parser");
        check(parse_openai_content(json::parse(R"({"choices":[{"message":{"content":"你好"}}]})"),false)=="你好","OpenAI nonstream parser");
        check(parse_openai_content(json::parse(R"({"choices":[],"usage":{}})"),true).empty(),"Usage-only chunk accepted");
        check(parse_openai_content(json::parse(R"({"choices":[{"delta":{"role":"assistant"}}]})"),true).empty(),"Role-only chunk accepted");
        rejects([]{parse_openai_content(json::parse(R"({"choices":["unexpected"]})"),false);},"Invalid choices shape rejected");
        rejects([]{parse_openai_content(json::parse(R"({"choices":[{"message":{"content":[{}]}}]})"),false);},"Nontext output rejected");
        {
            Settings openai;openai.provider="openai";const auto openai_key=translation_service_key(openai);
            for(const auto* url:{"https://api.openai.com/v1","https://api.openai.com/v1/","https://api.openai.com/v1/chat/completions","https://API.OPENAI.COM:443/v1/chat/completions/"}) {
                auto equivalent=openai;equivalent.base_url=url;
                check(translation_service_key(equivalent)==openai_key,"OpenAI default, slash and full endpoint share cooldown key");
            }
            auto changed=openai;changed.model="another-model";changed.target="ja";changed.api_key_env="ANOTHER_API_KEY";
            check(translation_service_key(changed)==openai_key,"Model, target and credential name do not bypass service cooldown");
            for(const auto* url:{"https://proxy.example.com/v1","https://api.openai.com/V1","http://api.openai.com/v1","https://api.openai.com:8443/v1"}) {
                changed=openai;changed.base_url=url;
                check(translation_service_key(changed)!=openai_key,"Host, case-sensitive path, scheme and port distinguish service destinations");
            }
            Settings ollama;ollama.provider="ollama";const auto ollama_key=translation_service_key(ollama);
            for(const auto* url:{"http://127.0.0.1:11434","http://127.0.0.1:11434/","http://127.0.0.1:11434/api/","http://127.0.0.1:11434/api/chat/"}) {
                auto equivalent=ollama;equivalent.base_url=url;
                check(translation_service_key(equivalent)==ollama_key,"Ollama default, slash and full endpoint share cooldown key");
            }
            check(ollama_key!=openai_key,"Different providers keep distinct service cooldown keys");
            Settings fixed;const auto google_key=translation_service_key(fixed);fixed.base_url="https://ignored.example/";fixed.model="unused";fixed.target="fr";
            check(translation_service_key(fixed)==google_key,"Google always uses its fixed service destination key");
            fixed.provider="demo";const auto demo_key=translation_service_key(fixed);fixed.base_url.clear();
            check(translation_service_key(fixed)==demo_key&&demo_key!=google_key,"Demo has a fixed service key distinct from Google");
        }
        std::atomic_bool cancel{false};Settings offline;offline.provider="demo";
        check(translate(offline,"All tests passed.",{},cancel)=="所有测试均已通过。","Offline demo translates fixed example");
        check(translate(offline,"Unrecognized sentence",{},cancel).find("离线演示")!=std::string::npos,"Offline unknown text explicitly labeled");
        Settings google;
        check(translate(google,"`app.cpp`",{},cancel)=="`app.cpp`","Inline code returned literally without network");
        check(translate(google,"```cpp\nreturn 0;\n```",{},cancel)=="```cpp\nreturn 0;\n```","Fenced code returned literally without network");
        check(translate(google,"~~~cpp\nreturn 0;",{},cancel)=="~~~cpp\nreturn 0;","Unclosed code fence never sent for translation");
        check(translate(offline," \n",{},cancel).empty(),"Whitespace input returns empty");rejects([&]{translate(offline,std::string(50001,'a'),{},cancel);},"Input size bounded");
        cancel=true;rejects([&]{translate(offline,"Hello",{},cancel);},"Cancellation before provider");cancel=false;
        {
            HttpServer server(R"({"choices":[{"message":{"content":"你好"}}]})");auto cfg=api(server);cfg.base_url+="/v1";cfg.api_key_env="CCLIVE_TEST_KEY";std::vector<std::string> deltas;
            check(translate(cfg,"Inspect C:\\repo\\app.cpp",[&](const std::string& d){deltas.push_back(d);},cancel)=="你好"&&deltas==std::vector<std::string>({"你好"}),"WinHTTP OpenAI JSON translation");
            server.wait();check(server.received.find("POST /v1/chat/completions ")!=std::string::npos&&server.received.find("Authorization: Bearer NEVER_SAVE_THIS_KEY")!=std::string::npos,"Endpoint and environment key sent only to configured host");
            auto h=server.received.find("\r\n\r\n");auto body=json::parse(server.received.substr(h+4));
            check(body["model"]=="test-model"&&body["stream"]==true&&!body.contains("tools")&&body["messages"][0]["content"].get<std::string>().find("file paths")!=std::string::npos,"Translation-only prompt preserves code and paths");
        }
        const std::string stream=": keepalive\n\ndata: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\ndata: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\r\n\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\n\ndata: [DONE]\n\n";
        {
            HttpServer server(stream,"text/event-stream; charset=utf-8");std::vector<std::string>deltas;
            check(translate(api(server),"Hello",[&](const std::string& d){deltas.push_back(d);},cancel)=="你好"&&deltas==std::vector<std::string>({"你","好"}),"SSE incremental Unicode deltas");
        }
        {
            HttpServer server("data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\n\n","text/event-stream");rejects([&]{translate(api(server),"Hello",{},cancel);},"Truncated SSE rejected");
        }
        for(const auto& response:std::vector<std::string>{"{\"message\":{\"content\":\"你\"},\"done\":false}\n{\"message\":{\"content\":\"好\"},\"done\":true}\n","{\"message\":{\"content\":\"你好\"},\"done\":true}"}){
            HttpServer server(response,"application/x-ndjson");auto cfg=api(server,"ollama");cfg.base_url+="/api";
            check(translate(cfg,"Hello",{},cancel)=="你好","Ollama NDJSON and single JSON accepted");server.wait();check(server.received.find("POST /api/chat ")!=std::string::npos,"Ollama API suffix normalized");
        }
        for(const auto& error:std::vector<std::string>{"not-json-SUPER_SECRET_SERVER_BODY","{\"error\":{\"message\":\"SUPER_SECRET_SERVER_BODY\"}}","{\"choices\":[\"SUPER_SECRET_SERVER_BODY\"]}"}){
            HttpServer server(error);std::string failure;try{translate(api(server),"Hello",{},cancel);}catch(const TranslationError& e){failure=e.what();}
            check(!failure.empty()&&failure.find("SUPER_SECRET_SERVER_BODY")==std::string::npos,"Provider errors never expose body");
        }
        {
            HttpServer server("SUPER_SECRET_SERVER_BODY","application/json",401);std::string error;try{translate(api(server),"Hello",{},cancel);}catch(const TranslationError&e){error=e.what();}
            check(error.find("401")!=std::string::npos&&error.find("SUPER_SECRET_SERVER_BODY")==std::string::npos,"HTTP errors safely summarized");
        }
        {
            auto rate_limit=[&](const std::string& headers,std::int64_t minimum,std::int64_t maximum,const std::string& provider="openai") {
                // A successful stream body would emit a delta if HTTP status handling read it.
                HttpServer server("data: {\"choices\":[{\"delta\":{\"content\":\"SUPER_SECRET_SERVER_BODY\"}}]}\n\ndata: [DONE]\n\n","text/event-stream",429,headers);
                std::vector<std::string> deltas;bool limited=false;std::int64_t delay=0;std::string message;
                try{translate(api(server,provider),"Hello",[&](const std::string& value){deltas.push_back(value);},cancel);}
                catch(const TranslationRateLimited& e){limited=true;delay=e.retry_after_ms;message=e.what();}
                catch(const TranslationError& e){message=e.what();}
                if(!limited||delay<minimum||delay>maximum)std::cerr<<"Retry-After fixture: "<<headers<<" delay="<<delay<<", expected "<<minimum<<".."<<maximum<<"\n";
                check(limited&&delay>=minimum&&delay<=maximum,"HTTP 429 exposes typed bounded Retry-After delay");
                check(message.find("429")!=std::string::npos&&message.find("请求过于频繁")!=std::string::npos&&message.find("SUPER_SECRET_SERVER_BODY")==std::string::npos,"HTTP 429 displays friendly summary without server body");
                check(deltas.empty(),"HTTP 429 never emits response body as translation delta");
                server.wait();check(server.requests==1,"HTTP 429 returns to caller without hidden retries");
            };
            rate_limit("Retry-After: 7\r\n",7000,7000);
            rate_limit("Retry-After: \t3 \t\r\n",3000,3000);
            rate_limit("Retry-After: 0\r\n",1000,1000);
            rate_limit("",60000,60000);
            rate_limit("Retry-After: invalid-date\r\n",60000,60000);
            rate_limit("Retry-After: nonsense 12:00:00 GMT\r\n",60000,60000);
            rate_limit("Retry-After: Sun, 99 Oct 2026 99:99:99 GMT\r\n",60000,60000);
            rate_limit("Retry-After: -2\r\n",60000,60000);
            rate_limit("Retry-After: 1.5\r\n",60000,60000);
            rate_limit("Retry-After: 999999999999999999999999999999999999\r\n",86400000,86400000);
            rate_limit("Retry-After: "+http_date_after(120)+"\r\n",110000,120000);
            rate_limit("Retry-After: "+http_date_after(-120)+"\r\n",1000,1000);
            rate_limit("Retry-After: "+http_date_after(2*24*60*60)+"\r\n",86400000,86400000);
            rate_limit("Retry-After: 9\r\n",9000,9000,"ollama");
        }
        {
            HttpServer server("","application/json",302,"Location: http://127.0.0.1:1/forbidden\r\n");std::string error;try{translate(api(server),"Hello",{},cancel);}catch(const TranslationError&e){error=e.what();}
            check(error.find("302")!=std::string::npos&&server.requests==1,"Redirects rejected without forwarding key or text");
        }
        {
            HttpServer server(stream,"text/event-stream");std::vector<std::string>deltas;bool cancelled=false;try{translate(api(server),"Hello",[&](const std::string&d){deltas.push_back(d);cancel=true;},cancel);}catch(const TranslationCancelled&){cancelled=true;}
            check(cancelled&&deltas==std::vector<std::string>({"你"}),"Cancel after first delta prevents later results");cancel=false;
        }
        {
            Settings bad;bad.provider="openai";rejects([&]{translate(bad,"Hello",{},cancel);},"Missing model rejected before network");bad.model="x";bad.base_url="https://user:pass@example.com";rejects([&]{translate(bad,"Hello",{},cancel);},"Credential-bearing URL rejected before network");bad.base_url="https://example.com?secret=value";rejects([&]{translate(bad,"Hello",{},cancel);},"Query URL rejected before network");bad.base_url="http://127.0.0.1:1";bad.api_key_env="CCLIVE_NOT_SET_KEY";SetEnvironmentVariableW(L"CCLIVE_NOT_SET_KEY",nullptr);rejects([&]{translate(bad,"Hello",{},cancel);},"Missing API key rejected before network");
        }
        {
            HttpServer server(std::string(2*1024*1024+1,'x'));rejects([&]{translate(api(server),"Hello",{},cancel);},"Oversized response rejected before reading body");
        }
        SetEnvironmentVariableW(L"CCLIVE_TEST_KEY",nullptr);
    }catch(const std::exception& e){++failures;std::cerr<<"UNEXPECTED: "<<e.what()<<"\n";}
    WSACleanup();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}


