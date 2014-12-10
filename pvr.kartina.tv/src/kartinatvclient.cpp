/*
 *      Copyright (C) 2013 Stanislav Ionascu
 *      Stanislav Ionascu
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "kartinatvclient.h"

#include <string>
#include <sstream>
#include <ostream>
#include <fstream>
#ifdef _MSC_VER
#include <WinSock2.h>
#else
#include <unistd.h>
#endif
#include <ctime>

#include <platform/sockets/tcp.h>
#include <json/json.h>
#include <libXBMC_addon.h>
#include <libXBMC_pvr.h>

namespace {
#define API_SERVER "iptv.kartina.tv"
#define API_URL "/api/json/"
const uint16_t API_PORT = 80;
const uint32_t REQ_TIME_LIMIT = 251000;

std::string makeApiUrl(const char *functionName)
{
    std::string result(API_URL);
    return result + functionName;
}

struct KTVError {
    KTVError() : code(0) { }

    int code;
    std::string message;
};

KTVError checkForError(std::string buffer)
{
    KTVError error;
    Json::Reader reader;
    Json::Value root;
    reader.parse(buffer, root);
    if (!root.isNull()) {
        Json::Value jsonError = root["error"];
        if (!jsonError.isNull()) {
            error.code = jsonError["code"].asInt();
            error.message = jsonError["message"].asString();
        }
    }
    return error;
}


struct UrlData {
    std::string hostname;
    std::string port;
    std::string query;
};

UrlData parseUrl(std::string url)
{
    UrlData data;
    data.query = url.substr(url.find('?') + 1);
    size_t hostnameStart = url.find("://") + 3;
    size_t hostnameEnd = url.find(':', hostnameStart) - 1;
    data.hostname = url.substr(hostnameStart, hostnameEnd - 6);
    data.port = "any";

    return data;
}
}

KartinaTVClient::KartinaTVClient(ADDON::CHelper_libXBMC_addon *XBMC, CHelper_libXBMC_pvr *PVR) :
    XBMC(XBMC), PVR(PVR), lastEpgQuery(0, 0)
{
}

KartinaTVClient::~KartinaTVClient()
{
    for (auto &c: channelEpgCache) {
        for (auto &t: c.second)
            delete t;
    }
}

bool KartinaTVClient::loadChannelGroupsFromCache(ADDON_HANDLE handle,
                                                 bool bRadio)
{
    for (const auto &group: channelGroupsCache) {
        if (!bRadio) {
            PVR_CHANNEL_GROUP channelGroup = createPvrChannelGroup(group);
            PVR->TransferChannelGroup(handle, &channelGroup);
        }
    }

    return true;
}

bool KartinaTVClient::loadChannelGroupMembersFromCache(
        ADDON_HANDLE handle,
        const PVR_CHANNEL_GROUP &group)
{
    for (const auto &groupMem: channelGroupMembersCache) {
        if (groupMem.name == group.strGroupName) {
            PVR_CHANNEL_GROUP_MEMBER member =
                    createPvrChannelGroupMember(groupMem);
            PVR->TransferChannelGroupMember(handle, &member);
        }
    }

    return true;
}

bool KartinaTVClient::loadChannelsFromCache(ADDON_HANDLE handle, bool bRadio)
{
    if (channelsCache.empty())
        updateChannelList();

    for (const auto &c: channelsCache) {
        if (c.isRadio == bRadio) {
            PVR_CHANNEL channel = createPvrChannel(c);
            PVR->TransferChannelEntry(handle, &channel);
        }
    }

    return true;
}

bool KartinaTVClient::loadEpgFromCache(ADDON_HANDLE handle, const PVR_CHANNEL &channel, time_t start, time_t end)
{
    XBMC->Log(ADDON::LOG_DEBUG, "KartinaTVClient::loadEpgFromCache %d %d %d",
              channel.iUniqueId,
              start,
              end);

    updateChannelEpg(channel.iUniqueId, start, end);
    auto epg = channelEpgCache.find(channel.iUniqueId);
    if (epg != channelEpgCache.cend()) {
        for (const auto &entry: epg->second)
            PVR->TransferEpgEntry(handle, entry);
    }
    return true;
}

std::string KartinaTVClient::requestStreamUrl(const PVR_CHANNEL &channel)
{
    XBMC->Log(ADDON::LOG_DEBUG, "void KartinaTVClient::requestStreamUrl()");

    bool isProtected = false;
    for (const auto &c: channelsCache) {
        if (c.id == channel.iUniqueId && c.isProtected)
            isProtected = true;
    }
    PostFields parameters;
    parameters.insert(std::make_pair("cid", std::to_string(channel.iUniqueId)));
    if (isProtected)
        parameters.insert(std::make_pair("protect_code", protectCode));

    std::string reply = makeRequest("get_url", parameters);
    XBMC->Log(ADDON::LOG_DEBUG, KTV_FUNC_INFO ": data: %s", reply.c_str());

    KTVError ktvError;
    if (reply.size() != 0 && (ktvError = checkForError(reply)).code == 0) {
        Json::Reader json;
        Json::Value root;
        json.parse(reply, root);
        const char *urlData = root["url"].asCString();

        std::vector<std::string> urlParams;
        std::stringstream stream(urlData);
        std::string param;
        while (stream >> param) {
            XBMC->Log(ADDON::LOG_DEBUG, "Extracted: %d %s", urlParams.size(), param.data());
            urlParams.push_back(param);
        }

        std::string url = urlParams.front();
        // http/ts
        url = url.replace(0, 7, "http");

        return url;
    } else {
        XBMC->Log(ADDON::LOG_ERROR, "Error occured: code %d %s", ktvError.code, ktvError.message.c_str());
        XBMC->Log(ADDON::LOG_ERROR, "Request to API server failed.");
    }

    return "";
}

void KartinaTVClient::setUserProfilePath(const std::string &path)
{
    userPath = path;
}

bool KartinaTVClient::login(const std::string &user, const std::string &pass)
{
    XBMC->Log(ADDON::LOG_DEBUG, "void KartinaTVClient::login()");

    PostFields parameters;
    parameters.insert(std::make_pair("login", user));
    parameters.insert(std::make_pair("pass", pass));
    std::string reply = makeRequest("login", parameters);

    KTVError ktvError;
    if (reply.size() != 0 && (ktvError = checkForError(reply)).code == 0) {
        Json::Reader json;
        Json::Value root;
        json.parse(reply, root);
        sessionId.first = root["sid_name"].asString();
        sessionId.second = root["sid"].asString();
    } else {
        XBMC->Log(ADDON::LOG_ERROR, "Error occured: code %d %s", ktvError.code, ktvError.message.c_str());
        XBMC->Log(ADDON::LOG_ERROR, "Request to API server failed.");
        return false;
    }

    return true;
}

void KartinaTVClient::logout()
{
    XBMC->Log(ADDON::LOG_DEBUG, "void KartinaTVClient::logout()");

    PostFields parameters;
    makeRequest("logout", parameters);
}

void KartinaTVClient::setProtectCode(const std::string &code)
{
    protectCode = code;
}

void KartinaTVClient::updateChannelList()
{
    XBMC->Log(ADDON::LOG_DEBUG, "void KartinaTVClient::updateChannelsList()");

    PostFields parameters;
    std::string reply = makeRequest("channel_list", parameters);
    channelsCache.clear();

    KTVError ktvError;
    if (reply.size() != 0 && (ktvError = checkForError(reply)).code == 0) {
        Json::Reader json;
        Json::Value root;
        json.parse(reply, root);
        Json::Value groups = root["groups"];
        for (Json::Value::UInt i = 0; i < groups.size(); ++i) {
            const Json::Value &group = groups[i];
            ChannelGroup channelGroup = channelGroupFromJson(group);
            channelGroupsCache.push_back(channelGroup);
            const Json::Value &channels = group["channels"];
            if (!channels.isNull()) {
                for (Json::Value::UInt j = 0; j < channels.size(); ++j) {
                    Channel channel = channelFromJson(channels[j]);
                    channel.number = channelsCache.size() + 1;
                    channelsCache.push_back(channel);
                    channelGroupMembersCache.push_back(
                                createChannelGroupMember(channel,
                                                         channelGroup));
                }
            }
        }
    } else {
        XBMC->Log(ADDON::LOG_ERROR, "Error occured: code %d %s",
                  ktvError.code, ktvError.message.c_str());
        XBMC->Log(ADDON::LOG_ERROR, "Request to API server failed.");
    }
}

void KartinaTVClient::updateChannelEpg(time_t start, int hours)
{
    XBMC->Log(ADDON::LOG_DEBUG, "KartinaTVClient::updateChannelEpg start=%d hours=%d", start, hours);
    PostFields parameters;
    parameters.insert(std::make_pair("dtime", std::to_string(start)));
    parameters.insert(std::make_pair("period", std::to_string(hours)));
    std::string reply = makeRequest("epg3", parameters);
    channelEpgCache.clear();

    KTVError ktvError;

    if (reply.size() != 0 && (ktvError = checkForError(reply)).code == 0) {
        Json::Reader json;
        Json::Value root;
        json.parse(reply, root);
        const Json::Value &epg3 = root["epg3"];
        for (Json::Value::UInt i = 0; i < epg3.size(); ++i) {
            const Json::Value &channelEpg = epg3[i];
            int channelId = -1;
            if (channelEpg["id"].isIntegral())
                channelId = channelEpg["id"].asInt();
            else
                channelId = std::stoi(channelEpg["id"].asString());

            const Json::Value &epg = channelEpg["epg"];
            EPG_TAG *lastEntry = NULL;
            if (!epg.isNull()) {
                for (Json::Value::UInt j = 0; j < epg.size(); ++j) {
                    const Json::Value &program = epg[j];

                    EPG_TAG *tag = new EPG_TAG;
                    memset(tag, 0, sizeof(EPG_TAG));
                    tag->iChannelNumber = channelId;
                    tag->iUniqueBroadcastId = channelId * 10000 + j;
                    tag->firstAired = 0;
                    if (lastEntry == NULL)
                        tag->startTime = start + 1;
                    else
                        tag->startTime = program["ut_start"].asInt();
                    tag->endTime = tag->startTime + 300;
                    if (lastEntry != NULL)
                        lastEntry->endTime = tag->startTime;
                    std::string longTitle = program["progname"].asString();
                    std::istringstream titleStream(longTitle);
                    std::vector<std::string> titleLines;
                    while (!titleStream.eof()) {
                        std::string line;
                        std::getline(titleStream, line);
                        titleLines.push_back(line);
                    }

                    std::string title, plot;

                    if (titleLines.size() > 0)
                        title = titleLines.at(0);
                    else
                        title = longTitle;

                    if (titleLines.size() > 1) {
                        for (unsigned int i = 1; i < titleLines.size(); ++i) {
                            plot += " " + titleLines.at(i);
                        }
                    }

                    tag->strTitle = _strdup(title.c_str());
                    tag->strPlot = _strdup(plot.c_str());

                    channelEpgCache[channelId].push_back(tag);
                    lastEntry = tag;
                }
            }

            if (lastEntry)
                lastEntry->endTime = lastEntry->startTime + 600;
        }
    } else {
        XBMC->Log(ADDON::LOG_ERROR, "Error occured: code %d %s", ktvError.code, ktvError.message.c_str());
        XBMC->Log(ADDON::LOG_ERROR, "Request to API server failed.");
    }
}

void KartinaTVClient::updateChannelEpg(int channelId, time_t start, time_t end)
{
    clearChannelEpg(channelId);

    char buf[32] = "";
    while (start <= end) {
        memset(&buf, 0, sizeof(buf));
        tm *t = std::localtime(&start);
        std::strftime(buf, 32, "%d%m%y", t);
        std::string day = buf;
        PostFields req = {
            { "cid", std::to_string(channelId) },
            { "day", day }
        };
        std::string reply = makeRequest("/epg", req);
        KTVError ktvError;
        if (reply.size() != 0 && (ktvError = checkForError(reply)).code == 0) {
            Json::Reader json;
            Json::Value root;
            json.parse(reply, root);
            const Json::Value &epg = root["epg"];
            int showId = 1;
            EPG_TAG *lastTag = NULL;
            for (Json::Value::UInt j = 0; j < epg.size(); ++ j) {
                const Json::Value &show = epg[j];
                EPG_TAG *tag = new EPG_TAG;
                memset(tag, 0, sizeof(EPG_TAG));
                tag->iChannelNumber = channelId;
                tag->iUniqueBroadcastId = channelId * 10000 + showId;
                tag->firstAired = 0;
                const auto &ut_start = show["ut_start"];
                if (ut_start.isInt())
                    tag->startTime = ut_start.asInt();
                else
                    tag->startTime = std::stoi(ut_start.asString());
                tag->endTime = tag->startTime + 300;
                if (lastTag != NULL)
                    lastTag->endTime = tag->startTime;
                std::string longTitle = show["progname"].asString();
                std::istringstream titleStream(longTitle);
                std::vector<std::string> titleLines;
                while (!titleStream.eof()) {
                    std::string line;
                    std::getline(titleStream, line);
                    titleLines.push_back(line);
                }

                std::string title, plot;
                if (titleLines.size() > 0)
                    title = titleLines.at(0);
                else
                    title = longTitle;

                if (titleLines.size() > 1) {
                    for (unsigned int i = 1; i < titleLines.size(); ++i) {
                        plot += " " + titleLines.at(i);
                    }
                }

                tag->strTitle = _strdup(title.c_str());
                tag->strPlot = _strdup(plot.c_str());

                channelEpgCache[channelId].push_back(tag);
                lastTag = tag;
                ++ showId;
            }
        }
        t->tm_mday += 1;
        start = mktime(t);
    }
}

void KartinaTVClient::clearChannelEpg(int channelId)
{
    for (auto c: channelEpgCache[channelId])
        delete c;
    channelEpgCache.erase(channelId);
}

KartinaTVClient::Channel KartinaTVClient::channelFromJson(const Json::Value &value)
{
    Channel channel;
    channel.id = value["id"].asInt();
    channel.name = value["name"].asString();
    channel.number = channel.id;
    channel.isRadio = value["is_video"].asInt() != 1;
    channel.iconUrl = std::string("http://" API_SERVER) +
            value["icon"].asString();
    channel.streamUrl = std::string("pvr://stream/tv/") +
            std::to_string(channel.id)
            + ".ts";
    channel.isProtected = value["protected"].asInt() == 1;

    return channel;
}

PVR_CHANNEL KartinaTVClient::createPvrChannel(
        const KartinaTVClient::Channel &channel)
{
    PVR_CHANNEL pvr;
    memset(&pvr, 0, sizeof(PVR_CHANNEL));

    pvr.bIsHidden = false;
    pvr.bIsRadio = channel.isRadio;
    pvr.iChannelNumber = channel.number;
    pvr.iUniqueId = channel.id;
    strcpy(pvr.strChannelName, channel.name.c_str());
    strcpy(pvr.strIconPath, channel.iconUrl.c_str());
    strcpy(pvr.strStreamURL, channel.streamUrl.c_str());

    return pvr;
}

KartinaTVClient::ChannelGroup KartinaTVClient::channelGroupFromJson(
        const Json::Value &value)
{
    ChannelGroup group;
    group.id = value["id"].asInt();
    group.name = value["name"].asString();

    return group;
}

PVR_CHANNEL_GROUP KartinaTVClient::createPvrChannelGroup(
        const KartinaTVClient::ChannelGroup &channelGroup)
{
    PVR_CHANNEL_GROUP group;
    memset(&group, 0, sizeof(PVR_CHANNEL_GROUP));

    group.bIsRadio = false;
    strcpy(group.strGroupName, channelGroup.name.c_str());

    return group;
}

KartinaTVClient::ChannelGroupMember KartinaTVClient::createChannelGroupMember(
        const KartinaTVClient::Channel &channel,
        const KartinaTVClient::ChannelGroup &group)
{
    ChannelGroupMember member;
    member.id = channel.id;
    member.number = channel.number;
    member.name = group.name;

    return member;
}

PVR_CHANNEL_GROUP_MEMBER KartinaTVClient::createPvrChannelGroupMember(
        const KartinaTVClient::ChannelGroupMember &member)
{
    PVR_CHANNEL_GROUP_MEMBER pvrMember;
    memset(&pvrMember, 0, sizeof(PVR_CHANNEL_GROUP_MEMBER));

    pvrMember.iChannelNumber = member.number;
    pvrMember.iChannelUniqueId = member.id;
    strcpy(pvrMember.strGroupName, member.name.c_str());

    return pvrMember;
}

std::string KartinaTVClient::makeRequest(const char *apiFunction, PostFields &parameters)
{
    XBMC->Log(ADDON::LOG_DEBUG, KTV_FUNC_INFO ": connecting to " API_SERVER "...");

    PLATFORM::CTcpConnection sock(API_SERVER, API_PORT);
    if (!sock.Open(30000)) {
        XBMC->Log(ADDON::LOG_ERROR, KTV_FUNC_INFO ": connection to " API_SERVER " failed!");
        return std::string();
    }

    XBMC->Log(ADDON::LOG_DEBUG, KTV_FUNC_INFO ": connected...");

    const std::string apiCallUrl = makeApiUrl(apiFunction);
    const std::string postFields = stringifyPostFields(parameters);
    CStdString request;
    request += "POST " + apiCallUrl + " HTTP/1.0" + "\r\n" +
            "Host: " API_SERVER + "\r\n" +
            "Content-Type: application/x-www-form-urlencoded" + "\r\n" +
            "Content-Length: " + std::to_string(postFields.size()) + "\r\n";
    if (!sessionId.second.empty()) {
        request += "Cookie: " + sessionId.first + "=" + sessionId.second + ";\r\n";
    }
    request += "\r\n";
    request += postFields + "\r\n";

    XBMC->Log(ADDON::LOG_DEBUG, postFields.data());

    XBMC->Log(ADDON::LOG_DEBUG, KTV_FUNC_INFO ": sending request %s",
              request.c_str());
    sock.Write(request.GetBuf(), request.size());

    std::string reply;
    while (sock.IsOpen()) {
        char buff[10240];
        ssize_t bytesRead = sock.Read(buff, sizeof(buff));
        if (bytesRead > 0) {
            reply +=  std::string(buff, bytesRead);
        } else {
            break;
        }
    }

    std::size_t httpDataStart = reply.find("\r\n\r\n");
    std::string headers = reply.substr(0, httpDataStart);
    std::string body;
    if (reply.size() > httpDataStart + 4)
        body = reply.substr(httpDataStart + 4, std::string::npos);

    XBMC->Log(ADDON::LOG_DEBUG, KTV_FUNC_INFO ": received headers %s",
              headers.c_str());

#ifdef _DEBUG
    for (int i = 0; i < body.size(); i += 1000) {
        XBMC->Log(ADDON::LOG_DEBUG, KTV_FUNC_INFO ": received reply %s",
                  body.substr(i, i + 1000 < body.size() ? 1000 : std::string::npos));
    }
#endif // KTV_DEBUG_REPLY

    sock.Close();

    XBMC->Log(ADDON::LOG_DEBUG, KTV_FUNC_INFO ": connection closed.");
    usleep(REQ_TIME_LIMIT);

    return body;
}

std::string KartinaTVClient::stringifyPostFields(const PostFields &fields)
{
    std::string postFields = "";
    for (const auto &f: fields) {
        if (postFields.length() > 0)
            postFields += "&";
        postFields += f.first + "=" + f.second;
    }

    return postFields;
}
