#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <stdexcept>
namespace caelum::mods {
struct Requirement { std::string sha256, name, url; };
// Lower-case host of an http(s) URL ("" when there is none), as direct_download_url and the client compare it.
inline std::string url_host(const std::string& url) {
  const auto scheme=url.find("://");if(scheme==std::string::npos)return {};
  const auto start=scheme+3,end=(std::min)(url.find_first_of("/?#",start),url.size());
  std::string host=url.substr(start,end-start);for(auto& c:host)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
  return host;
}
// Share links of file hosts open a web page with a Download button, not the file. direct_download_url maps them to
// the host's direct download; other URLs come back unchanged. Folder and document links throw: they are no ZIP.
//  * Google Drive (drive.google.com/file/d/ID/view, open?id=ID, uc?id=ID, docs.google.com/uc?id=ID,
//    drive.usercontent.google.com/download?id=ID) -> drive.usercontent.google.com/download?id=ID&export=download
//    &confirm=t; confirm=t skips the virus-scan page Drive shows for large files, resourcekey is kept (pre-2021 links).
//  * Dropbox (www.dropbox.com/scl/fi/..., /s/...) -> the same link with dl=1 (Dropbox's documented forced download);
//    rlkey/st/e are kept.
//  * OneDrive personal (1drv.ms/..., onedrive.live.com/...) -> the OneDrive API's shared-item content,
//    api.onedrive.com/v1.0/shares/u!<base64url of the link>/root/content (Microsoft's documented share-id encoding).
//    Microsoft refuses anonymous access there for some accounts since 2025; the client then reports that.
//  * OneDrive for Business / SharePoint (*.sharepoint.com/:u:/...) -> the link with ?download=1.
inline std::string direct_download_url(const std::string& url) {
  const std::string host=url_host(url);if(host.empty())return url;
  const auto end=(std::min)(url.find_first_of("/?#",url.find("://")+3),url.size());
  const std::string rest=url.substr(end);const auto cut=rest.find_first_of("?#");
  const std::string path=rest.substr(0,cut),query=cut!=std::string::npos&&rest[cut]=='?'?rest.substr(cut+1,rest.find('#',cut)-cut-1):"";
  auto params=[&]{std::vector<std::string> out;std::istringstream parts(query);std::string part;while(std::getline(parts,part,'&'))if(!part.empty())out.push_back(part);return out;};
  auto param=[&](const std::string& key){for(const auto& part:params())if(part.starts_with(key+"="))return part.substr(key.size()+1);return std::string();};
  auto token=[](const std::string& s){return s.size()<=128&&s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos;};
  auto url_text=[](const std::string& s){for(unsigned char c:s)if(c<33||c>126||std::string("\"<>\\^`{|}").find(char(c))!=std::string::npos)return false;return true;};
  auto has=[&](const char* part){return path.find(part)!=std::string::npos;};
  if(host=="drive.google.com"||host=="docs.google.com"||host=="drive.usercontent.google.com") {
    if(has("/folders/"))throw std::runtime_error("Google Drive folder links cannot be downloaded; share the ZIP file itself");
    for(const char* kind:{"/document/","/spreadsheets/","/presentation/","/forms/"})
      if(has(kind))throw std::runtime_error("Google Docs links are not ZIP files; share the ZIP file itself");
    const auto d=path.find("/d/");
    const std::string id=d!=std::string::npos?path.substr(d+3,path.find('/',d+3)-d-3):param("id"),key=param("resourcekey");
    if(id.empty()||!token(id)||!token(key))throw std::runtime_error("Unrecognized Google Drive link");
    return "https://drive.usercontent.google.com/download?id="+id+"&export=download&confirm=t"+(key.empty()?"":"&resourcekey="+key);
  }
  if(host=="dropbox.com"||host=="www.dropbox.com") {
    if(path.starts_with("/scl/fo/")||path.starts_with("/sh/"))throw std::runtime_error("Dropbox folder links cannot be downloaded; share the ZIP file itself");
    if(!path.starts_with("/scl/fi/")&&!path.starts_with("/s/"))return url;
    std::string kept;
    for(const auto& part:params())
      if(!part.starts_with("dl=")&&!part.starts_with("raw=")&&part!="dl"&&part!="raw")kept+=part+"&";
    if(!url_text(path)||!url_text(kept))throw std::runtime_error("Unrecognized Dropbox link");
    return "https://www.dropbox.com"+path+"?"+kept+"dl=1";
  }
  if(host=="1drv.ms"||host=="onedrive.live.com") {
    if(host=="1drv.ms"&&path.starts_with("/f/"))throw std::runtime_error("OneDrive folder links cannot be downloaded; share the ZIP file itself");
    const std::string link=url.substr(0,url.find('#'));
    if(!url_text(link)||link.size()>1500)throw std::runtime_error("Unrecognized OneDrive link");
    static const char* abc="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string id="u!";unsigned bits=0;int count=0;
    for(unsigned char c:link){bits=(bits<<8)|c;count+=8;while(count>=6){count-=6;id+=abc[(bits>>count)&63];}}
    if(count)id+=abc[(bits<<(6-count))&63];
    return "https://api.onedrive.com/v1.0/shares/"+id+"/root/content";
  }
  if(host.size()>15&&host.ends_with(".sharepoint.com")) {
    if(has("/:f:/"))throw std::runtime_error("SharePoint folder links cannot be downloaded; share the ZIP file itself");
    if(!url_text(host+path))throw std::runtime_error("Unrecognized SharePoint link");
    return "https://"+host+path+"?download=1";
  }
  return url;
}
inline std::vector<Requirement> parse_manifest(std::string text) {
  auto require=[](bool ok,const char* error){if(!ok)throw std::runtime_error(error);};
  require(text.size()<=30000,"Server mod list exceeds 30 KB");
  while(!text.empty()&&text.back()==0)text.pop_back();
  if(text.starts_with("CMOD1\r\n"))text.erase(5,1);
  require(text.starts_with("CMOD1\n"),"Unsupported server mod protocol");
  std::istringstream lines(text.substr(6));std::string line;std::vector<Requirement> result;std::set<std::string> hashes;
  while(std::getline(lines,line)) {
    if(!line.empty()&&line.back()=='\r')line.pop_back();
    if(line.empty())continue;
    const auto a=line.find('\t'),b=a==std::string::npos?a:line.find('\t',a+1);
    require(a==64&&b!=std::string::npos&&line.find('\t',b+1)==std::string::npos,"Invalid server mod list");
    Requirement r{line.substr(0,a),line.substr(a+1,b-a-1),line.substr(b+1)};
    require(r.name.size()>0&&r.name.size()<=128&&r.url.size()<=2048&&result.size()<32,"Invalid server mod name, URL or count");
    require(r.sha256.find_first_not_of("0123456789abcdef")==std::string::npos&&hashes.insert(r.sha256).second,"Invalid or duplicate mod SHA256");
    require(r.url.starts_with("https://")||r.url.starts_with("http://"),"Mod URL must use HTTP or HTTPS");
    for(unsigned char c:line)require(c>=32||c=='\t',"Invalid server mod list character");
    direct_download_url(r.url); // folder/document share links fail here, on the server before sending and on the client
    result.push_back(std::move(r));
  }
  return result;
}
}
