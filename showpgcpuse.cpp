#include <stdio.h>
#include <map>
#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>

using std::string; using std::cout; using std::endl; using std::map; using std::vector;

std::string ltrim( const std::string& s ) {
    size_t startpos = s.find_first_not_of(" \n\r\t");
    return (startpos == std::string::npos) ? "" : s.substr(startpos);
}

std::string rtrim( const std::string& s ) {
    size_t endpos = s.find_last_not_of(" \n\r\t");
    return (endpos == std::string::npos) ? "" : s.substr(0, endpos+1);
}

std::string trim( const std::string& s ) {
    return rtrim(ltrim(s));
}

std::vector<string> explode( const string& s, const string& separ ) {
	std::vector<string> resu; string token("");
	for ( auto const &c: s ) {
		if (separ.find_first_of(c)==string::npos) token+=c;
		else { resu.push_back(token); token=""; }
	}
	if (token.size()) resu.push_back(token);
	return resu;
}

long long total_for(const map<string,long long>& ventil)
{
	long long resu=0;
	for (auto const &it: ventil) resu+=it.second;
	return resu;
}

void disp_stat(const string& title, const map<string,long long>& ventil)
{
	cout << title << endl;
	long long tot=total_for(ventil);
	for (auto const &it: ventil)
	{
		cout << it.first << "\t" << it.second << "\t" << (it.second*100)/tot << "%" << endl;
	}
	cout << endl;
}

int main(int argc, const char *argv[])
{
	if (argc<2) { printf("syntax: %s <stats_file>\n",argv[0]); return 1; }
	std::ifstream in(argv[1]);
	map<string,long long> cpu_by_db, cpu_by_user, cpu_by_from;
	long long cpu_total=0;
	map<string,unsigned int> proc_by_db, proc_by_user, proc_by_from;
	unsigned int proc_total=0;
	while (!in.eof())
	{
		string line;
		std::getline(in,line);
		line=trim(line);
		if (line.substr(0,5)!="START")
		{
			//cout << ">" << line << "<" << endl;
			vector<string> fld=explode(line,"\t");
			if (fld.size()>=7)
			{
				string db=fld[4],user=fld[5],from=fld[6];
				//cout << "db=" << db << "user=" << user << endl;
				long long cpused=atoll(fld[3].c_str());
				cpu_by_db[db]+=cpused; cpu_by_user[user]+=cpused; cpu_by_from[from]+=cpused;
				++proc_by_db[db]; ++proc_by_user[user]; ++proc_by_from[from];
				cpu_total+=cpused; ++proc_total;
			}
		}
	}
	disp_stat("CPU BY DB:",cpu_by_db);
	disp_stat("CPU BY USER:",cpu_by_user);
	disp_stat("CPU BY ORIGIN:",cpu_by_from);
	//TODO: total cpu time

	return 0;
}

