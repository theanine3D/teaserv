#pragma once
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <stdexcept>
namespace caelum::mods {
struct Requirement { std::string sha256, name, url; };
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
    result.push_back(std::move(r));
  }
  return result;
}
}
