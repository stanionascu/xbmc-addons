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

#include <curl/curl.h>
#include <json/json.h>
#include <libXBMC_addon.h>
#include <libXBMC_pvr.h>

namespace {
const char *API_SERVER = "http://iptv.kartina.tv/api/json/";

std::string makeApiUrl(const char *functionName)
{
    std::string result(API_SERVER);
    return result + functionName;
}

size_t curlWriteToMemory(void *buffer, size_t size, size_t nitems, void *outstream)
{
    size_t bufferSize = size * nitems;
    if (!outstream)
        return bufferSize;

    KartinaTVClient::CurlMemoryBlob *blob = static_cast<KartinaTVClient::CurlMemoryBlob*>(outstream);

    blob->buffer = static_cast<char*>(realloc(blob->buffer, blob->size + bufferSize + 1));
    memcpy(&(blob->buffer[blob->size]), buffer, bufferSize);
    blob->size += bufferSize;
    blob->buffer[blob->size] = 0;

    return bufferSize;
}

template<class T>
std::string toString(T any) { std::stringstream stream; stream << any; return stream.str(); }

struct KTVError {
    KTVError() : code(0) { }

    int code;
    std::string message;
};

KTVError checkForError(const char *buffer)
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
    XBMC(XBMC), PVR(PVR), curl(0), lastEpgQuery(0, 0)
{
}

KartinaTVClient::~KartinaTVClient()
{
    curl_easy_cleanup(curl);
}

bool KartinaTVClient::loadChannelGroupsFromCache(ADDON_HANDLE handle,
                                                 bool bRadio)
{
    std::list<ChannelGroup>::const_iterator it = channelGroupsCache.begin();
    for (; it != channelGroupsCache.end(); ++it) {
        if (!bRadio) {
            PVR_CHANNEL_GROUP channelGroup = createPvrChannelGroup(*it);
            PVR->TransferChannelGroup(handle, &channelGroup);
        }
    }

    return true;
}

bool KartinaTVClient::loadChannelGroupMembersFromCache(
        ADDON_HANDLE handle,
        const PVR_CHANNEL_GROUP &group)
{
    std::list<ChannelGroupMember>::const_iterator it =
            channelGroupMembersCache.begin();
    for (; it != channelGroupMembersCache.end(); ++it) {
        if (strcmp(group.strGroupName, (*it).name.c_str()) == 0) {
            PVR_CHANNEL_GROUP_MEMBER member = createPvrChannelGroupMember(*it);
            PVR->TransferChannelGroupMember(handle, &member);
        }
    }

    return true;
}

bool KartinaTVClient::loadChannelsFromCache(ADDON_HANDLE handle, bool bRadio)
{
    if (channelsCache.empty())
        updateChannelList();

    std::list<Channel>::const_iterator it = channelsCache.begin();
    for (; it != channelsCache.end(); ++it) {
        if ((*it).isRadio == bRadio) {
            PVR_CHANNEL channel = createPvrChannel(*it);
            PVR->TransferChannelEntry(handle, &channel);
        }
    }

    return true;
}

bool KartinaTVClient::loadEpgFromCache(ADDON_HANDLE handle, const PVR_CHANNEL &channel, time_t start, time_t end)
{
    XBMC->Log(ADDON::LOG_DEBUG, "KartinaTVClient::loadEpgFromCache %d", start);

    // if new query is at least 60s in the future, update cache.
    if ((start - lastEpgQuery.first) > 60) {
        updateChannelEpg(start + 3600 * 18, (end - start)/3600 + 3);
        lastEpgQuery = std::make_pair(start, end);
    }

    if (channelEpgCache.count(channel.iUniqueId) > 0) {
        const std::list<EPG_TAG*> &epg = channelEpgCache.at(channel.iUniqueId);
        std::list<EPG_TAG*>::const_iterator it = epg.begin();
        for (; it != epg.end(); ++it) {
            PVR->TransferEpgEntry(handle, *it);
        }
    }
    return true;
}

std::string KartinaTVClient::requestStreamUrl(const PVR_CHANNEL &channel)
{
    XBMC->Log(ADDON::LOG_DEBUG, "void KartinaTVClient::requestStreamUrl()");

    bool isProtected = false;
    std::list<Channel>::const_iterator it = channelsCache.begin();
    for (; it != channelsCache.end(); ++ it ) {
        if (it->id == channel.iUniqueId && it->isProtected)
            isProtected = true;
    }
    PostFields parameters;
    parameters.insert(std::make_pair("cid", toString(channel.iUniqueId)));
    if (isProtected)
        parameters.insert(std::make_pair("protect_code", protectCode));

    CurlMemoryBlob reply = makeRequest("get_url", parameters);
    XBMC->Log(ADDON::LOG_DEBUG, "%s: data: %s", __FUNCTION__,
              XBMC->UnknownToUTF8(reply.buffer));

    KTVError ktvError;
    if (reply.size != 0 && (ktvError = checkForError(reply.buffer)).code == 0) {
        Json::Reader json;
        Json::Value root;
        json.parse(reply.buffer, root);
        const char *urlData = root["url"].asCString();

        std::list<std::string> urlParams;
        std::stringstream stream(urlData);
        std::string param;
        while (stream >> param) {
            XBMC->Log(ADDON::LOG_DEBUG, "Extracted: %d %s", urlParams.size(), param.data());
            urlParams.push_back(param);
        }

        std::string url = urlParams.front();
        // http/ts
        url = url.replace(0, 7, "http");

        free(reply.buffer);
        return url;
    } else {
        XBMC->Log(ADDON::LOG_ERROR, "Error occured: code %d %s", ktvError.code, ktvError.message.c_str());
        XBMC->Log(ADDON::LOG_ERROR, "Request to API server failed.");
        free(reply.buffer);
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
    CurlMemoryBlob reply = makeRequest("login", parameters);

    KTVError ktvError;
    if (reply.size != 0 && (ktvError = checkForError(reply.buffer)).code == 0) {
        XBMC->Log(ADDON::LOG_DEBUG, reply.buffer);

        Json::Reader json;
        Json::Value root;
        json.parse(reply.buffer, root);
        sessionId.first = root["sid_name"].asString();
        sessionId.second = root["sid"].asString();
        free(reply.buffer);
    } else {
        XBMC->Log(ADDON::LOG_ERROR, "Error occured: code %d %s", ktvError.code, ktvError.message.c_str());
        XBMC->Log(ADDON::LOG_ERROR, "Request to API server failed.");

        free(reply.buffer);
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
    CurlMemoryBlob reply = makeRequest("channel_list", parameters);
    channelsCache.clear();

    KTVError ktvError;
    if (reply.size != 0 && (ktvError = checkForError(reply.buffer)).code == 0) {
        Json::Reader json;
        Json::Value root;
        json.parse(reply.buffer, root);
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

    free(reply.buffer);
}

void KartinaTVClient::updateChannelEpg(time_t start, int hours)
{
    XBMC->Log(ADDON::LOG_DEBUG, "KartinaTVClient::updateChannelEpg start=%d hours=%d", start, hours);
    PostFields parameters;
    parameters.insert(std::make_pair("dtime", toString(start)));
    parameters.insert(std::make_pair("period", toString(hours)));
    CurlMemoryBlob reply = makeRequest("epg3", parameters);
    channelEpgCache.clear();

    KTVError ktvError;

    if (reply.size != 0 && (ktvError = checkForError(reply.buffer)).code == 0) {
        Json::Reader json;
        Json::Value root;
        json.parse(reply.buffer, root);
        const Json::Value &epg3 = root["epg3"];
        for (Json::Value::UInt i = 0; i < epg3.size(); ++i) {
            const Json::Value &channelEpg = epg3[i];
            int channelId = -1;
            if (channelEpg["id"].isIntegral())
                channelId = channelEpg["id"].asInt();
            else
                channelId = std::stoi(channelEpg["id"].asString());

            if (channelEpgCache.count(channelId) == 0)
                channelEpgCache.insert(std::make_pair(channelId, std::list<EPG_TAG*>()));

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

                    channelEpgCache.at(channelId).push_back(tag);
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

    free(reply.buffer);
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
    channel.streamUrl = std::string("pvr://stream/tv/") + toString(channel.id)
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

KartinaTVClient::CurlMemoryBlob KartinaTVClient::makeRequest(const char *apiFunction, PostFields &parameters)
{
    if (!curl)
        curl = curl_easy_init();

    XBMC->Log(ADDON::LOG_DEBUG, "void KartinaTVClient::makeRequest()");

    CurlMemoryBlob reply;
    reply.buffer = static_cast<char*>(malloc(1));
    reply.size = 0;
    reply.buffer[0] = 0;

    const std::string apiCallUrl = makeApiUrl(apiFunction);

    curl_easy_setopt(curl, CURLOPT_URL, apiCallUrl.data());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToMemory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&reply);

    std::string cookiesPath = userPath + "cookies.txt";
    XBMC->Log(ADDON::LOG_DEBUG, "User path for cookies: %s", cookiesPath.data());
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookiesPath.data());
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookiesPath.data());

    const std::string postFields = stringifyPostFields(parameters);

    XBMC->Log(ADDON::LOG_DEBUG, postFields.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.data());

    CURLcode result = curl_easy_perform(curl);

    XBMC->Log(ADDON::LOG_DEBUG, "void KartinaTVClient::makeRequest()");
    if (result == CURLE_OK)
        return reply;

    return CurlMemoryBlob();
}

std::string KartinaTVClient::stringifyPostFields(const PostFields &fields)
{
    std::string postFields = "";
    PostFields::const_iterator it = fields.begin();
    for (; it != fields.end(); ++it) {
        if (postFields.length() > 0)
            postFields += "&";
        postFields += it->first + "=" + it->second;
    }

    return postFields;
}
