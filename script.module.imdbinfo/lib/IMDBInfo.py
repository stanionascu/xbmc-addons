# -*- coding: utf-8 -*-
""" IMDBInfo
    Script for manually fetching IMDB movie information by name in XBMC add-ons
    Copyright (C) 2011 Stanislav Ionascu <stanislav.ionascu@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
"""

import urllib, urllib2, re, xbmc, os, md5
import simplejson as json

__author__ = "Stanislav Ionascu (stanislav.ionascu@gmail.com)"
__version__ = "0.0.1"
__copyright__ = "Copyright (c) 2011 Stanislav Ionascu"
__license__ = "GPLv3"

IMDB_API_SERVER = "http://www.imdbapi.com/"

cacheFolder = os.path.join(xbmc.translatePath('special://temp/'), '');

class IMDBInfo:
	"""Class for parsing kinopoisk.ru"""

	def __init__(self):
		"""Set variables: version of class, default search url, headers for requests"""
		self.headers = dict({	
					'User-Agent': 'IMDBInfo script module for XBMC/%s' % __version__,
					'Referer': 'http://www.imdbapi.com/',
					'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
					'Host': 'www.imdbapi.com',
					'Accept-Language': 'en-US,en;q=0.8'
		});
			
	def search(self, title):
		cacheFile = os.path.join(xbmc.translatePath('special://temp/'), 'imdb_%s.json' % md5.new(title).hexdigest());
		cacheCoverFile = os.path.join(xbmc.translatePath('special://temp/'), 'imdb_cover_%s.jpg' % md5.new(title).hexdigest());
		movieInfoRaw = "";
		print cacheFile;
		print cacheCoverFile;
		if os.path.exists(cacheFile) == False:
			response = self.get('',
				{
					't': title
				}
			)
			movieInfoRaw = response.read();
			fd = os.open(cacheFile, os.O_WRONLY);
			os.write(fd, movieInfoRaw);
			os.close(fd);
		else:
			fd = os.open(cacheFile, os.O_RDONLY);
			movieInfoRaw = os.read(fd, os.path.getsize(cacheFile));
			os.close(fd);
		
		movieInfo = dict(json.loads(movieInfoRaw));
		if "Poster" in movieInfo:
			if movieInfo["Poster"] != "N/A":
				if os.path.exists(cacheCoverFile) == False:
					headers = dict({	
						'User-Agent': 'IMDBInfo script module for XBMC/%s' % __version__,
						'Referer': 'http://www.imdb.com/',
						'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
						'Host': 'ia.media-imdb.com',
						'Accept-Language': 'en-US,en;q=0.8'
					});
					coverResponse = self.get(url="", server=movieInfo["Poster"], headers=headers);
					fd = os.open(cacheCoverFile, os.O_WRONLY);
					os.write(fd, coverResponse.read());
					os.close(fd);
				movieInfo["Poster"] = cacheCoverFile
		
		return movieInfo;
		
	def get(self, url, params = {}, server = '', headers = {}):
		if len(server) == 0:
			server = IMDB_API_SERVER;
		requestURL = '%s%s' % (server, url);
		if len(params) > 0:
			requestURL += '?' + urllib.urlencode(params);
		
		if len(headers) == 0:
			headers = self.headers;
			
		request = urllib2.Request(url=requestURL, headers=headers);
		print requestURL;
		response = urllib2.urlopen(request);
		return response;

if __name__ == '__main__':
	import sys
	scraper = IMDBInfo()
	print scraper.search(sys.argv[1]);
