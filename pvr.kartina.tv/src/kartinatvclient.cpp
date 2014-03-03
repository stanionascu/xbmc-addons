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
#include <json/json_tokener.h>
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

const char *stringFromJsonObject(json_object *object, const char *key) { return json_object_get_string(json_object_object_get(object, key)); }
int intFromJsonObject(json_object *object, const char *key) { return json_object_get_int(json_object_object_get(object, key)); }
array_list *arrayFromJsonObject(json_object *object, const char *key) { return json_object_get_array(json_object_object_get(object, key)); }
json_object *objectFromJsonArray(array_list *array, int index) { return (json_object*)array_list_get_idx(array, index); }

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
    json_object *obj = json_tokener_parse(buffer);
    if (obj) {
        json_object *jError = json_object_object_get(obj, "error");
        if (jError) {
            error.code = intFromJsonObject(jError, "code");
            error.message = stringFromJsonObject(jError, "message");
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


int waitForSocket(curl_socket_t socket, long msecs, bool receive)
{
    timeval tv;
    tv.tv_sec = msecs / 1000;
    tv.tv_usec = (msecs % 1000) * 1000;

    fd_set infd, outfd, errfd;
    FD_ZERO(&infd);
    FD_ZERO(&outfd);
    FD_ZERO(&errfd);

    FD_SET(socket, &errfd);

    if (receive)
        FD_SET(socket, &infd);
    else
        FD_SET(socket, &outfd);

    int result = select(socket + 1, &infd, &outfd, &errfd, &tv);
    return result;
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
//    XBMC->Log(ADDON::LOG_NOTICE, "KartinaTVClient::loadEpgFromCache for channel: %d (has: %d)", channel.iChannelNumber,
//              channelEpgCache.count(channel.iChannelNumber));

    if (lastEpgQuery != std::make_pair(start, end)) {
        updateChannelEpg(start + 3600 * 18, (end - start)/3600 + 3);
        lastEpgQuery = std::make_pair(start, end);
    }

    XBMC->Log(ADDON::LOG_NOTICE, "KartinaTVClient::loadEpgFromCache");

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
    XBMC->Log(ADDON::LOG_NOTICE, "void KartinaTVClient::requestStreamUrl()");

    PostFields parameters;
    parameters.insert(std::make_pair("cid", toString(channel.iUniqueId)));

    CurlMemoryBlob reply = makeRequest("get_url", parameters);

    KTVError ktvError;
    if (reply.size != 0 && (ktvError = checkForError(reply.buffer)).code == 0) {
        XBMC->Log(ADDON::LOG_NOTICE, reply.buffer);
        json_object *obj = json_tokener_parse(reply.buffer);
        const char *urlData = stringFromJsonObject(obj, "url");

        std::list<std::string> urlParams;
        std::stringstream stream(urlData);
        std::string param;
        while (stream >> param) {
            XBMC->Log(ADDON::LOG_NOTICE, "Extracted: %d %s", urlParams.size(), param.data());
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
    XBMC->Log(ADDON::LOG_NOTICE, "void KartinaTVClient::login()");

    PostFields parameters;
    parameters.insert(std::make_pair("login", user));
    parameters.insert(std::make_pair("pass", pass));
    CurlMemoryBlob reply = makeRequest("login", parameters);

    KTVError ktvError;
    if (reply.size != 0 && (ktvError = checkForError(reply.buffer)).code == 0) {
        XBMC->Log(ADDON::LOG_NOTICE, reply.buffer);

        json_object *obj = json_tokener_parse(reply.buffer);
        sessionId.first = stringFromJsonObject(obj, "sid_name");
        sessionId.second = stringFromJsonObject(obj, "sid");
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
    XBMC->Log(ADDON::LOG_NOTICE, "void KartinaTVClient::logout()");

    PostFields parameters;
    makeRequest("logout", parameters);
}

void KartinaTVClient::updateChannelList()
{
    XBMC->Log(ADDON::LOG_NOTICE, "void KartinaTVClient::updateChannelsList()");

    PostFields parameters;
    CurlMemoryBlob reply = makeRequest("channel_list", parameters);
    channelsCache.clear();

    KTVError ktvError;
    if (reply.size != 0 && (ktvError = checkForError(reply.buffer)).code == 0) {
        json_object *obj = json_tokener_parse(reply.buffer);
        array_list *groups = arrayFromJsonObject(obj, "groups");
        for (int i = 0; i < array_list_length(groups); ++i) {
            json_object *group = objectFromJsonArray(groups, i);
            ChannelGroup channelGroup = channelGroupFromJson(group);
            channelGroupsCache.push_back(channelGroup);
            json_object *channels = json_object_object_get(group, "channels");
            if (channels) {
                array_list *channelsList = json_object_get_array(channels);
                for (int j = 0; j < array_list_length(channelsList); ++j) {
                    Channel channel = channelFromJson(
                                objectFromJsonArray(channelsList, j));
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
    XBMC->Log(ADDON::LOG_NOTICE, "KartinaTVClient::updateChannelEpg start=%d hours=%d", start, hours);
    PostFields parameters;
    parameters.insert(std::make_pair("dtime", toString(start)));
    parameters.insert(std::make_pair("period", toString(hours)));
    CurlMemoryBlob reply = makeRequest("epg3", parameters);
    channelEpgCache.clear();

    KTVError ktvError;

    if (reply.size != 0 && (ktvError = checkForError(reply.buffer)).code == 0) {
        json_object *obj = json_tokener_parse(reply.buffer);
        array_list *epg3 = arrayFromJsonObject(obj, "epg3");
        for (int i = 0; i < array_list_length(epg3); ++i) {
            json_object *channelEpg = objectFromJsonArray(epg3, i);
            int channelId = intFromJsonObject(channelEpg, "id");
            char *channelName = XBMC->UnknownToUTF8(stringFromJsonObject(channelEpg, "name"));
            if (channelEpgCache.count(channelId) == 0)
                channelEpgCache.insert(std::make_pair(channelId, std::list<EPG_TAG*>()));

            json_object *epg = json_object_object_get(channelEpg, "epg");
            EPG_TAG *lastEntry = NULL;
            if (epg) {
                array_list *epgList = json_object_get_array(epg);
                for (int j = 0; j < array_list_length(epgList); ++j) {
                    json_object *program = (json_object*)array_list_get_idx(epgList, j);

                    EPG_TAG *tag = new EPG_TAG;
                    memset(tag, 0, sizeof(EPG_TAG));
                    tag->iChannelNumber = channelId;
                    tag->iUniqueBroadcastId = channelId * 10000 + j;
                    tag->firstAired = 0;
                    if (lastEntry == NULL)
                        tag->startTime = start + 1;
                    else
                        tag->startTime = intFromJsonObject(program, "ut_start");
                    tag->endTime = tag->startTime + 300;
                    if (lastEntry != NULL)
                        lastEntry->endTime = tag->startTime;
                    std::string longTitle = XBMC->UnknownToUTF8(stringFromJsonObject(program, "progname"));
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

                    tag->strTitle = XBMC->UnknownToUTF8(title.data());
                    tag->strPlot = XBMC->UnknownToUTF8(plot.data());

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

KartinaTVClient::Channel KartinaTVClient::channelFromJson(json_object *obj)
{
    Channel channel;
    channel.id = intFromJsonObject(obj, "id");
    channel.name = XBMC->UnknownToUTF8(stringFromJsonObject(obj, "name"));
    channel.number = channel.id;
    channel.isRadio = (intFromJsonObject(obj, "is_video") != 1);
    channel.iconUrl = std::string("http://iptv.kartina.tv") +
            XBMC->UnknownToUTF8(stringFromJsonObject(obj, "icon"));
    channel.streamUrl = std::string("pvr://stream/tv/") + toString(channel.id)
            + ".ts";

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
        json_object *obj)
{
    ChannelGroup group;
    group.id = intFromJsonObject(obj, "id");
    group.name = XBMC->UnknownToUTF8(stringFromJsonObject(obj, "name"));

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

    XBMC->Log(ADDON::LOG_NOTICE, "void KartinaTVClient::makeRequest()");

    CurlMemoryBlob reply;
    reply.buffer = static_cast<char*>(malloc(1));
    reply.size = 0;
    reply.buffer[0] = 0;

    const std::string apiCallUrl = makeApiUrl(apiFunction);

    XBMC->Log(ADDON::LOG_NOTICE, apiCallUrl.data());
    curl_easy_setopt(curl, CURLOPT_URL, apiCallUrl.data());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToMemory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&reply);

    std::string cookiesPath = userPath + "cookies.txt";
    XBMC->Log(ADDON::LOG_ERROR, "User path for cookies: %s", cookiesPath.data());
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookiesPath.data());
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookiesPath.data());

    const std::string postFields = stringifyPostFields(parameters);

    XBMC->Log(ADDON::LOG_NOTICE, postFields.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.data());

    CURLcode result = curl_easy_perform(curl);

    XBMC->Log(ADDON::LOG_NOTICE, "void KartinaTVClient::makeRequest()");
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
