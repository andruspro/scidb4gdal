/*
Copyright (c) 2016 Marius Appel <marius.appel@uni-muenster.de>

This file is part of scidb4gdal. scidb4gdal is licensed under the MIT license.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
-----------------------------------------------------------------------------*/

#include "shimclient.h"
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <cctype> // Microsoft Visual C++ compatibility
#include "TemporalReference.h"
#include <iomanip>
#include <limits>

namespace scidb4gdal {
    using namespace scidb4geo;

    ShimClient::ShimClient()
        : _host("https://localhost"), _port(8083), _user("scidb"), _passwd("scidb"), _ssl(true), _curl_handle(0), _curl_initialized(false), _auth(""),  _hasSCIDB4GEO(NULL), _shimversion("") {
        curl_global_init(CURL_GLOBAL_ALL);
        stringstream ss;

        // Add http:// or https:// to the URL if needed
        if ((_host.substr(0, 8).compare("https://") != 0) && (_host.substr(0, 7).compare("http://"))) {
            ss << (_ssl ? "https://" : "http://");
        }
        ss << _host;

    #ifdef CURL_ADDPORTTOURL
        /* 2016-05-10: Fix problems with digest authentification. The port is simply
        * added to the base URL because
        * libcurl automatically uses default ports (443,80) in 2nd digest auth
        * requests (as a result of 401 responses). */
        ss << ":" << _port;
    #endif
        _host = ss.str();
    }

    ShimClient::ShimClient(string host, uint16_t port, string user, string passwd,
                        bool ssl = false)
        : _host(host), _port(port), _user(user), _passwd(passwd), _ssl(ssl), _curl_handle(0), _curl_initialized(false), _auth(""),  _hasSCIDB4GEO(NULL), _shimversion("") {
        curl_global_init(CURL_GLOBAL_ALL);
        stringstream ss;

        // Add http:// or https:// to the URL if needed
        if ((_host.substr(0, 8).compare("https://") != 0) && (_host.substr(0, 7).compare("http://"))) {
            ss << (_ssl ? "https://" : "http://");
        }
        ss << _host;

    #ifdef CURL_ADDPORTTOURL
        /* 2016-05-10: Fix problems with digest authentification. The port is simply
        * added to the base URL because
        * libcurl automatically uses default ports (443,80) in 2nd digest auth
        * requests (as a result of 401 responses). */
        ss << ":" << _port;
    #endif

        host = ss.str();
    }

    ShimClient::ShimClient(ConnectionParameters* con) : 
        _host(con->host), 
        _port(con->port),
        _user(con->user),
        _passwd(con->passwd),
        _ssl(con->ssl),
        _ssltrust(con->ssltrust),
        _curl_handle(0),
        _curl_initialized(false),
        _auth(""), 
        _hasSCIDB4GEO(NULL),
        _shimversion(""){
        
        curl_global_init(CURL_GLOBAL_ALL);

        stringstream ss;

        // Add http:// or https:// to the URL if needed
        if ((_host.substr(0, 8).compare("https://") != 0) && (_host.substr(0, 7).compare("http://"))) {
            ss << (_ssl ? "https://" : "http://");
        }
        ss << _host;

    #ifdef CURL_ADDPORTTOURL
        /* 2016-05-10: Fix problems with digest authentification. The port is simply
        * added to the base URL because
        * libcurl automatically uses default ports (443,80) in 2nd digest auth
        * requests (as a result of 401 responses). */
        ss << ":" << _port;
    #endif
        _host = ss.str();
    }

    ShimClient::~ShimClient() {
        if (_ssl && !_auth.empty())
            logout();
        curl_global_cleanup();
        _curl_handle = 0;
        if (_hasSCIDB4GEO !=  NULL) delete _hasSCIDB4GEO;
    }

    /**
    * Handles the cURL callback by creating a string from it.
    */
    static size_t responseToStringCallback(void* ptr, size_t size, size_t count,
                                        void* stream) {
        ((string*)stream)->append((char*)ptr, 0, size * count);
        return size * count;
    }

    static size_t responseSilentCallback(void* ptr, size_t size, size_t count,
                                        void* stream) {
        return size * count;
    }

    /**
    * Callback function for receiving scidb binary data
    */
    static size_t responseBinaryCallback(void* ptr, size_t size, size_t count,
                                        void* stream) {
        size_t realsize = size * count;
        struct SingleAttributeChunk* mem = (struct SingleAttributeChunk*)stream;
        memcpy(&(mem->memory[mem->size]), ptr, realsize);
        mem->size += realsize;
        return realsize;
    }

    void ShimClient::curlBegin() {
        if (_curl_initialized)
            curlEnd();

        _curl_handle = curl_easy_init();
        _curl_initialized = true;
        // curl_easy_setopt ( _curl_handle, CURLOPT_URL, _host.c_str() );
        curl_easy_setopt(_curl_handle, CURLOPT_PORT, _port);
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(_curl_handle, CURLOPT_USERNAME, _user.c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_PASSWORD, _passwd.c_str());

        if (_ssl && _ssltrust) {
            curl_easy_setopt(_curl_handle, CURLOPT_SSL_VERIFYPEER, 0);
            curl_easy_setopt(_curl_handle, CURLOPT_SSL_VERIFYHOST, 0);
        }
        else if (_ssl && !_ssltrust) {
            curl_easy_setopt(_curl_handle, CURLOPT_SSL_VERIFYPEER, 1);
            curl_easy_setopt(_curl_handle, CURLOPT_SSL_VERIFYHOST, 1);
        } 
        
    #ifdef CURL_VERBOSE
        curl_easy_setopt(_curl_handle, CURLOPT_VERBOSE, 1L);
    #else
        // default silent, otherwise weird number output on stdout
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                        &responseSilentCallback);
    #endif
    }

    void ShimClient::curlEnd() {
        if (_curl_initialized) {
            curl_easy_cleanup(_curl_handle);
            _curl_initialized = false;
        }
    }

    CURLcode ShimClient::curlPerform() {
        CURLcode res = curl_easy_perform(_curl_handle);

        /* 2016-04-27: Added a second perform() for HTTP digest auth */
        long response_code;
        curl_easy_getinfo(_curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
        if (response_code == 401)
            res = curl_easy_perform(_curl_handle);

        for (int i = 1; i < CURL_RETRIES && res == CURLE_COULDNT_CONNECT; ++i) {
            stringstream s;
            s << "Connection error, retrying ... "
            << "(#" << i << ")";
            Utils::warn(s.str());
            Utils::sleep(i * 100);
            res = curl_easy_perform(_curl_handle);
        }
        if (res != CURLE_OK) {
            Utils::error((string)("curl_easy_perform() failed: ") +
                        curl_easy_strerror(res));
        }
        return res;
    }

    StatusCode ShimClient::testConnection() {
        curlBegin();

        stringstream ss;
        ss << _host << SHIMENDPOINT_VERSION;
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "?auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());

        // Test connection
        string response;
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                        &responseToStringCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);

        curlPerform();
        curlEnd();
        Utils::debug("SHIM Version: " + response);

        return SUCCESS;
    }

    
    
    string ShimClient::getVersion()
    {
        if (!_shimversion.empty()) return _shimversion;
        
        Utils::debug("Requesting the server's SciDB / shim version over HTTP...");
        curlBegin();

        stringstream ss;
        ss << _host << SHIMENDPOINT_VERSION;
       
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());

        // Test connection
        string response;
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,&responseToStringCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);

        curlPerform();
        curlEnd();
        
        _shimversion = response;
        
        Utils::debug("SciDB server runs version " + response);

        return response;
    }
    
    
    void ShimClient::stringToVersion(const string& version, int* major, int* minor)
    {
        vector<string> parts;
        boost::split(parts, version, boost::is_any_of(".-"));
        if (parts.size() < 2) {
            stringstream ss; 
            ss << "Cannot extract SciDB / shim version from string '" << version << "'";
            Utils::error(ss.str());
            major = 0;
            minor = 0;
            return;
        }
        *major = boost::lexical_cast<int>(parts[0].substr(1,parts[0].length()-1)); // Remove "v" prefix
        *minor = boost::lexical_cast<int>(parts[1]);
      
    }
    
    
    bool ShimClient::isVersionGreaterThan(int maj, int min) {
        int major,minor;
        stringToVersion(getVersion(),&major,&minor);
        
        if (major > maj || (major == maj && minor > min)) 
        {
            return true;
        }   
        return false;
    }

    
    
    
    bool ShimClient::hasSCIDB4GEO()
    {
        StatusCode s = SUCCESS;
        bool x = hasSCIDB4GEO(s);
        if (s !=  SUCCESS) 
            Utils::error("Cannot check whether SciDB server has spacetime extension.");
        return x;
    }
        
        
    bool ShimClient::hasSCIDB4GEO(StatusCode &ret)
    {
        ret = SUCCESS;
        if (_hasSCIDB4GEO !=  NULL) return *_hasSCIDB4GEO; 
 
        Utils::debug("Checking whether SciDB server runs spacetime extensions...");
        stringstream ss, afl;

        int sessionID = newSession();

        // There might be less complex queries but this one always succeeds and does
        // not give HTTP 500 SciDB errors
        afl << "aggregate(filter(list('libraries'), name='libscidb4geo.so'), count(name))";
        Utils::debug("Performing AFL Query: " + afl.str());

        ss.str();
        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?" << "id=" << sessionID << "&query=" << curl_easy_escape(_curl_handle,afl.str().c_str() , 0) << "&save=" << "(uint64)";
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;

        curlBegin();
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        if ( curlPerform() !=  CURLE_OK) {
            Utils::error("Error while reading binary data from query result");
            ret = ERR_GLOBAL_UNKNOWN;
            curlEnd();
            releaseSession(sessionID);
            return false;
        }
            
        curlEnd();

        curlBegin();
        // READ BYTES  ////////////////////////////
        ss.str("");
        ss << _host << SHIMENDPOINT_READ_BYTES << "?" << "id=" << sessionID << "&n=0";
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

        struct SingleAttributeChunk data;
        // Expect just one integer
        data.memory = (char*)malloc(sizeof(uint64_t) * 1);
        data.size = 0;

        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION, responseBinaryCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, (void*)&data);
        if ( curlPerform() !=  CURLE_OK) {
            ret = ERR_GLOBAL_UNKNOWN;
            curlEnd();
            releaseSession(sessionID);
            return false;
        }
        curlEnd();

        uint64_t count = ((uint64_t*)data.memory)[0];
        
        free(data.memory);
        releaseSession(sessionID);
        ret = SUCCESS;
        bool* c = new bool; 
        *c = count > 0;
        _hasSCIDB4GEO = c;  
   
        if (*c) {
           Utils::debug("Spacetime extension found. Good.");
        }
        else {
            Utils::debug("Spacetime extension not found.");
        }
        return *_hasSCIDB4GEO;
    }

    
    
    
    int ShimClient::newSession() {
        
        
        
        if (_ssl && _auth.empty())
            login();

        curlBegin();

        stringstream ss;
        string response;

        // NEW SESSION ID ////////////////////////////
        ss << _host << SHIMENDPOINT_NEW_SESSION;
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "?auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

        // Test connection

        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                        &responseToStringCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);

        curlPerform();

        curlEnd();

        // int sessionID = boost::lexical_cast<int>(response.data());
        int sessionID = atoi(response.c_str());
        if (sessionID > 0)
            return sessionID;

        Utils::error((string)("Invalid session ID"));
        return -1;
    }

    void ShimClient::releaseSession(int sessionID) {
        curlBegin();
        stringstream ss;
        ss << _host << SHIMENDPOINT_RELEASE_SESSION;
        ss << "?"
        << "id=" << sessionID;
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        string response;
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                        &responseToStringCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
        curlPerform();
        curlEnd();
    }

    void ShimClient::login() {
        
        /* Since the login endpoint as been removed with SciDB 15.12 we need
           to check the version first and set _auth to an arbitrary value. This 
           value is passed to all following HTTP requests but will not be 
           interpreted by shim. This workaround remains backward compatibility
           with older shim versions. */
        
        if (isVersionGreaterThan(15,7)) {
            _auth="UNUSED";
            return;
        }
        
        
        curlBegin();
        
        

        stringstream ss;
        string response;

        ss << _host << SHIMENDPOINT_LOGIN << "?username=" << _user
        << "&password=" << _passwd;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION, &responseToStringCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
        curlPerform();
        curlEnd();

        // int sessionID = boost::lexical_cast<int>(response.data());
        if (response.length() > 0) {
            _auth = response;
            stringstream auth_enc;
            for (int i = 0; i<_auth.length(); ++i) auth_enc <<  "x";
            Utils::debug((string) "Login to SciDB successsful, using auth key: " + auth_enc.str());
        } else {
            Utils::error((string)("Login to SciDB failed"), true);
        }
    }

    void ShimClient::logout() {
        
        
         /* Since the logout endpoint as been removed with SciDB 15.12 we need
           to check the version first and set _auth to an arbitrary value. This 
           value is passed to all following HTTP requests but will not be 
           interpreted by shim. This workaround remains backward compatibility
           with older shim versions. */
        
        int major,minor;
        stringToVersion(getVersion(),&major,&minor);
        
        if (major > 15 || (major == 15 && minor >= 12)) 
        {
            _auth="UNUSED";
            return;
        }
        
        curlBegin();

        stringstream ss;

        // NEW SESSION ID ////////////////////////////
        ss << _host << SHIMENDPOINT_LOGOUT << "?auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

        curlPerform();
        curlEnd();
    }

    StatusCode ShimClient::getAttributeDesc(const string& inArrayName,
                                            vector<SciDBAttribute>& out) {
        out.clear();

        int sessionID = newSession();

        string response;
        stringstream ss;

        /*
        * Make a request to fetch the attributes of the data set
        */
        {
            stringstream afl;
            ss.str("");
            curlBegin();
            afl << "project(attributes(" << inArrayName << "),name,type_id,nullable)";
            Utils::debug("Performing AFL Query: " + afl.str());
            ss << _host << SHIMENDPOINT_EXECUTEQUERY;
            ss << "?"
            << "id=" << sessionID << "&query=" << afl.str() << "&save="
            << "csv";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;

            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);

            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::error("Cannot get attribute information for array '" +
                            inArrayName + "'. Does it exist?");
                return ERR_READ_UNKNOWN;
            }
            curlEnd();
        }

        /**
        * Read data from the request and save it at the variable "response"
        */
        {
            curlBegin();
            ss.str("");
            // READ BYTES  ///////////////////////////
            ss << _host << SHIMENDPOINT_READ_BYTES << "?" << "id=" << sessionID << "&n=0";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::error("Cannot get attribute information for array '" +
                            inArrayName + "'. Does it exist?");
                return ERR_READ_UNKNOWN;
            }
            curlEnd();
        }

      
        ss.str("");
        
        
        CSVstring* csv;
        if (!isVersionGreaterThan(15,7)) {
         csv = new CSVstring( response,true); // with header  
        }
        else csv = new CSVstring( response,false); // with header  
        
        
        for (int i=0; i<csv->nrow(); ++i) 
        {
             SciDBAttribute attr;
             attr.name = csv->get<string>(i,0).substr(1, csv->get<string>(i,0).length() - 2); // remove ''
             attr.typeId = csv->get<string>(i,1).substr(1, csv->get<string>(i,1).length() - 2); // remove ''
             attr.nullable = ((csv->get<string>(i,2).compare("TRUE") * csv->get<string>(i,2).compare("true")) == 0);
             
              // Assert attr has datatype that is supported by GDAL
                if (Utils::scidbTypeIdToGDALType(attr.typeId) == GDT_Unknown) {
                    ss.str("");
                    ss << "SciDB GDAL driver does not support data type " << attr.typeId << ". Array attribute '" << attr.name << "' will be ignored." ;
                    Utils::warn(ss.str());
                    continue;
                }
                out.push_back(attr);
        }
        
        delete csv;
        
        releaseSession(sessionID);
        
        if (out.size() == 0) {
            Utils::error("Array '" + inArrayName + "' has no valid GDAL compatible attributes.");
            return ERR_GLOBAL_UNKNOWN;
        }

        return SUCCESS;
    }

    StatusCode ShimClient::getDimensionDesc(const string& inArrayName,
                                            vector<SciDBDimension>& out) {
        out.clear();

        int sessionID = newSession();

        string response;

        /*
        * Request
        */
        {
            curlBegin();
            stringstream ss;
            stringstream afl;
            // project(dimensions(chicago2),name,low,high,type)
            afl << "project(dimensions(" << inArrayName << "),name,low,high,type,chunk_interval,start,length)";
            Utils::debug("Performing AFL Query: " + afl.str());

            ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
            << "id=" << sessionID << "&query=" << afl.str() << "&save="
            << "csv";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::error("Cannot get dimension information for array '" +
                            inArrayName + "'. Does it exist?");
                return ERR_READ_UNKNOWN;
            }
            curlEnd();
        }

        /*
        * Get response
        */
        {
            curlBegin();
            stringstream ss;
            response = "";
            ss << _host << SHIMENDPOINT_READ_BYTES << "?"
            << "id=" << sessionID << "&n=0";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::error("Cannot get dimension information for array '" +
                            inArrayName + "'. Does it exist?");
                return ERR_READ_UNKNOWN;
            }
            curlEnd();
        }

        // Parse CSV
       

        
        
        
        CSVstring* csv;
        if (!isVersionGreaterThan(15,7)) {
         csv = new CSVstring( response,true); // with header  
        }
        else csv = new CSVstring( response,false); // with header  
        
        
        for (int i=0; i<csv->nrow(); ++i) 
        {   
            SciDBDimension dim;
            dim.name = csv->get<string>(i,0).substr(1, csv->get<string>(i,0).length() - 2);
            dim.low = boost::lexical_cast<int64_t>(csv->get<string>(i,1).c_str());
            dim.high = boost::lexical_cast<int64_t>(csv->get<string>(i,2).c_str());
            // Remove quotes
            dim.typeId = csv->get<string>(i,3).substr(1, csv->get<string>(i,3).length() - 2);
            dim.chunksize = boost::lexical_cast<uint32_t>(csv->get<string>(i,4).c_str());
            dim.start = boost::lexical_cast<int64_t>(csv->get<string>(i,5).c_str());
            dim.length = boost::lexical_cast<int64_t>(csv->get<string>(i,6).c_str());

            // yet unspecified e.g. for newly created arrays
            if (dim.high == SCIDB_MAX_DIM_INDEX || dim.low == SCIDB_MAX_DIM_INDEX ||
                dim.high == -SCIDB_MAX_DIM_INDEX || dim.low == -SCIDB_MAX_DIM_INDEX) {
                dim.low = boost::lexical_cast<int64_t>(csv->get<string>(i,5).c_str());
                dim.high = dim.low + boost::lexical_cast<int64_t>(csv->get<string>(i,6).c_str()) - 1;
            }

            // Assert  dim.typeId is integer
            if (!(dim.typeId == "int32" || dim.typeId == "int64" ||
                dim.typeId == "int16" || dim.typeId == "int8" ||
                dim.typeId == "uint32" || dim.typeId == "uint64" ||
                dim.typeId == "uint16" || dim.typeId == "uint8")) {
                stringstream ss;
                ss << "SciDB GDAL driver works with integer dimensions only. Got dimension " << dim.name << ":" << dim.typeId;
                Utils::error(ss.str());
                return ERR_READ_UNKNOWN;
            }

            out.push_back(dim);
               
        }
        
        
        
        delete csv;
        
        releaseSession(sessionID);

        return SUCCESS;
    }

    StatusCode ShimClient::getSRSDesc(const string& inArrayName, SciDBSpatialReference& out) {
                                        
        if (!hasSCIDB4GEO()) { //  Default spatial reference
            out.affineTransform = *(new AffineTransform());
            out.auth_name = "UNDEFINED";
            out.auth_srid = 0;
            out.proj4text = "";
            out.srtext = "";
            out.xdim = "x";
            out.ydim = "y";
            return SUCCESS;
        }
            
        int sessionID = newSession();
        string response;

        {
            curlBegin();
            stringstream ss;
            stringstream afl;
            // project(dimensions(chicago2),name,low,high,type)
            afl << "project(apply(st_getsrs(" << inArrayName << "),auth_srid_str,string(auth_srid)),name,xdim,ydim,srtext,proj4text,A,auth_name,auth_srid_str)"; // Everything should be a string to smiplify CSV col separation
            Utils::debug("Performing AFL Query: " + afl.str());
            ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
            << "id=" << sessionID << "&query=" << afl.str() << "&save="
            << "csv";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::warn("Cannot find spatial reference information for array '" + inArrayName + "'");
                out.affineTransform = *(new AffineTransform());
                out.auth_name = "UNDEFINED";
                out.auth_srid = 0;
                out.proj4text = "";
                out.srtext = "";
                out.xdim = "x";
                out.ydim = "y";
                return SUCCESS;
            };
            curlEnd();
        }

        {
            curlBegin();
            stringstream ss;
            // READ BYTES  ////////////////////////////
            ss.str("");
            ss << _host << SHIMENDPOINT_READ_BYTES << "?"
            << "id=" << sessionID << "&n=0";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;

            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION, &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::warn("Cannot read spatial reference information for array '" +
                            inArrayName + "'");
                return ERR_SRS_NOSPATIALREFFOUND;
            };
            curlEnd();
        }

        // Parse CSV

        CSVstring* csv;
        if (!isVersionGreaterThan(15,7)) {
         csv = new CSVstring( response,"','","\n", true); // with header  
        }
        else csv =  new CSVstring( response,"','","\n", false); // without header  
        
        
        
        if (csv->nrow() != 1 || csv->ncol() != 8) 
        {
            delete csv;
            Utils::error("Cannot extract spatial reference of array '" + inArrayName + "'.");
            return ERR_GLOBAL_PARSE;
        }
        
        
        
        try {
            
            out.xdim = csv->get<string>(0,1);
            out.ydim = csv->get<string>(0,2);
            out.srtext = csv->get<string>(0,3);
            out.proj4text = csv->get<string>(0,4);
            out.affineTransform = *(new AffineTransform(csv->get<string>(0,5)));
            out.auth_name = csv->get<string>(0,6);
            out.auth_srid = boost::lexical_cast<uint32_t>(csv->get<string>(0,7).substr(0,csv->get<string>(0,7).length()-1)); // last colum has ' ending
        }
        catch (const boost::bad_lexical_cast &) 
        {
            delete csv;
            Utils::warn("Cannot extract spatial reference of array '" + inArrayName + "'.");
            return ERR_GLOBAL_PARSE;  
        }
 
        
        delete csv;

        releaseSession(sessionID);

        return SUCCESS;
    }

    StatusCode ShimClient::getTRSDesc(const string& inArrayName,
                                    SciDBTemporalReference& out) {
        int sessionID = newSession();
        string response;

        // st_gettrs query preparation
        {
            curlBegin();
            stringstream ss;
            stringstream afl;
            afl << "project(st_gettrs(" << inArrayName << "),tdim,t0,dt)";
            Utils::debug("Performing AFL Query: " + afl.str());
            ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
            << "id=" << sessionID << "&query=" << afl.str() << "&save="
            << "csv";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::warn("Cannot find spatial reference information for array '" + inArrayName + "'");
                return ERR_SRS_NOSPATIALREFFOUND;
            };
            curlEnd();
        }

        // st_gettrs call
        {
            curlBegin();
            stringstream ss;
            // READ BYTES  ////////////////////////////
            ss.str("");
            ss << _host << SHIMENDPOINT_READ_BYTES << "?"
            << "id=" << sessionID << "&n=0";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;

            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::warn("Cannot read temporal reference information for array '" +
                            inArrayName + "'");
                return ERR_SRS_NOSPATIALREFFOUND;
            };
            curlEnd();
        }

        
        
        CSVstring* csv;
        if (!isVersionGreaterThan(15,7)) {
         csv = new CSVstring( response,true); // with header  
        }
        else csv = new CSVstring( response,false); // with header  
        
        
        if (csv->nrow() != 1 || csv->ncol() != 3) 
        {
            delete csv;
            Utils::warn("Cannot extract temporal reference of array '" + inArrayName + "'.");
            return ERR_GLOBAL_PARSE;
        }
        
        
        
        out.tdim = csv->get<string>(0,0).substr(1,csv->get<string>(0,0).length()-2); // remove ''
        TPoint* p = new TPoint(csv->get<string>(0,1).substr(1,csv->get<string>(0,1).length()-2)); // remove '';
        TInterval* i = new TInterval(csv->get<string>(0,2).substr(1,csv->get<string>(0,2).length()-2)); // remove '';
        out.setTPoint(p);
        out.setTInterval(i);
            
        delete csv;
        releaseSession(sessionID);

        return SUCCESS;  
    }
    
    
    

    StatusCode ShimClient::getArrayDesc(const string& inArrayName,
                                        SciDBSpatialArray*& out) {
        bool exists;
        arrayExists(inArrayName, exists);
        if (!exists) {
            Utils::error("Array '" + inArrayName + "' does not exist in SciDB database");
            return ERR_READ_ARRAYUNKNOWN;
        }

        getType(inArrayName, out);
        if (!out) {
            Utils::debug("No array was created");
        }
        out->name = inArrayName;
        // Get dimensions: project(dimensions(inArrayName),name,low,high,type)
        StatusCode res;
        res = getDimensionDesc(inArrayName, out->dims);
        if (res != SUCCESS) {
            Utils::error("Cannot extract array dimension metadata");
            return res;
        }

        // Get attributes: project(attributes(inArrayName),name,type_id,nullable);
        res = getAttributeDesc(inArrayName, out->attrs);
        if (res != SUCCESS) {
            Utils::error("Cannot extract array attribute metadata");
            return res;
        }

        
        Utils::debug("SciDB array schema: " + out->getSchemaString()); 
        
        /*
        * Make calls for metadata. First try to get the spatial reference system,
        * then try to get the temporal rs
        */
        getSRSDesc(inArrayName, *out);

        SciDBSpatioTemporalArray* starr_ptr =
            dynamic_cast<SciDBSpatioTemporalArray*>(out);
        if (starr_ptr) {
            getTRSDesc(inArrayName, *starr_ptr);
        }

        // set the metadata
        MD m;
        getArrayMD(m, inArrayName, "");
        out->md.insert(pair<string, MD>("", m)); // TODO: Add domain

        for (size_t i = 0; i < out->attrs.size(); ++i) {
            MD ma;
            // TODO: Add domain
            getAttributeMD(ma, inArrayName, out->attrs[i].name, "");
            // TODO: Add domain
            out->attrs[i].md.insert(pair<string, MD>("", ma));
        }

        return SUCCESS;
    }

    StatusCode ShimClient::getType(const string& name, SciDBSpatialArray*& array) {
       

        StatusCode s;
        bool check = hasSCIDB4GEO(s);
        if (s !=  SUCCESS) {
            Utils::error("Cannot check whether SciDB server has spacetime extension.");
            return ERR_GLOBAL_UNKNOWN;
        }
        if  (!check) {   
            Utils::warn("The SciDB server currently does not run the spacetime extension 'scidb4geo'. To support geographic reference storage and related features please install the extension to the server.");
            array = new SciDBSpatialArray();
            return SUCCESS;
        }
        
        int sessionID = newSession();
        string response;
        
        {
            curlBegin();
            stringstream ss;
            stringstream afl;
            afl << "project(filter(eo_arrays(), name='" <<  name <<  "'),name,setting)";
            Utils::debug("Performing AFL Query: " + afl.str());
            ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
            << "id=" << sessionID << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0) << "&save=" << "csv";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION, &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::warn("Cannot query for spatial or temporal annotated query. "
                            "SCIDB4GEO module activated in SCIDB?");
                return ERR_GLOBAL_NO_SCIDB4GEO;
            };
            curlEnd();
        }

        {
            curlBegin();
            stringstream ss;
            // READ BYTES  ////////////////////////////
            ss.str("");
            ss << _host << SHIMENDPOINT_READ_BYTES << "?"
            << "id=" << sessionID << "&n=0";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;

            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                return ERR_GLOBAL_UNKNOWN;
            };
            
            curlEnd();
        }

        
        
        CSVstring* csv;
        if (!isVersionGreaterThan(15,7)) {
         csv = new CSVstring( response,true); // with header  
        }
        else csv = new CSVstring( response,false); // without header  
        
        
        if (csv->nrow() != 1 || csv->ncol() < 1) 
        {
            delete csv;
            Utils::warn("Cannot extract setting of array '" + name + "'.");
            return ERR_GLOBAL_PARSE;
        }
        
    
        if ( csv->get<string>(0,1).compare("'s'") == 0) {
            array = new SciDBSpatialArray();    
        } 
        else if ( csv->get<string>(0,1).compare("'st'") == 0) {
            array = new SciDBSpatioTemporalArray();
        } 
        else 
        {
            Utils::error("Cannot derive setting for array '" + name + "'. Invalid response of project(filter(eo_arrays(...))).");
            releaseSession(sessionID);
            delete csv;
            return ERR_GLOBAL_UNKNOWN;
        }
        delete csv;
        return SUCCESS;
    }

    StatusCode ShimClient::getData(SciDBSpatialArray& array, uint8_t nband,
                                void* outchunk, int32_t x_min, int32_t y_min,
                                int32_t x_max, int32_t y_max, bool use_subarray,
                                bool emptycheck) {
        int t_index;
        if (x_min < array.getXDim()->low || x_min > array.getXDim()->high ||
            x_max < array.getXDim()->low || x_max > array.getXDim()->high ||
            y_min < array.getYDim()->low || y_min > array.getYDim()->high ||
            y_max < array.getYDim()->low || y_max > array.getYDim()->high) {
            Utils::error("Requested array subset is outside array boundaries");
        }

        if (nband >= array.attrs.size())
            Utils::error("Requested array band does not exist");

        int8_t x_idx = array.getXDimIdx();
        int8_t y_idx = array.getYDimIdx();

        stringstream ss;
        string response;
        
        
        
        MD md = array.attrs[nband].md[""];
        string naval;
        if (md.find(SCIDB4GDAL_DEFAULTMDFIELD_NODATA) == md.end())
        {
            stringstream dtos;
            dtos <<  Utils::defaultNoDataSciDB(array.attrs[nband].typeId);
            naval = dtos.str();
        }
        else naval =  md[SCIDB4GDAL_DEFAULTMDFIELD_NODATA];

  
        

        int sessionID = newSession();

        /* Depending on dimension ordering, array must be transposed. */

        curlBegin();
        // EXECUTE QUERY  ////////////////////////////
        ss.str();
        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"  << "id=" << sessionID;

        stringstream tslice;
        if (SciDBSpatioTemporalArray* starray =
                dynamic_cast<SciDBSpatioTemporalArray*>(&array)) {
            // if we have a temporal index, we need to slice the data set
            if (_qp) {
                t_index = _qp->temp_index;
            } else if (_cp) {
                TPoint temp_point = TPoint(_cp->timestamp);
                t_index = starray->indexAtDatetime(temp_point);
            } else {
                // TODO throw error
                t_index = 0;
                Utils::debug("Neither query nor creation parameter were found.");
            }

            tslice << "slice(" << array.name << "," + starray->tdim + "," << t_index
                << ")";
        } else {
            // Utils::debug("Cast failed. Skipping the slicing.");
            tslice << array.name;
        }

        string arr = tslice.str();

        stringstream afl;
        if (x_idx > y_idx) { // TODO: need to check performance of differend ordering

            if (use_subarray) {
                if (emptycheck) {
                    afl << "(merge(";
                    afl << "project(subarray(" << arr << "," << y_min << "," << x_min << ","
                        << y_max << "," << x_max << ")," << array.attrs[nband].name << ")";
                    afl << ",build(<" << array.attrs[nband].name << ":"
                        << array.attrs[nband].typeId << ((array.attrs[nband].nullable) ? " NULL" : " NOT NULL")
                        << "> [" << array.getYDim()->name
                        << "=" << 0 << ":" << y_max - y_min << ","
                        << array.getYDim()->chunksize << "," << 0 << ","
                        << array.getXDim()->name << "=" << 0 << ":" << x_max - x_min << ","
                        << array.getXDim()->chunksize << "," << 0 << "],"
                        << naval << ")))";
                } else {
                    afl << "(project(subarray(" << arr << "," << y_min << "," << x_min
                        << "," << y_max << "," << x_max << ")," << array.attrs[nband].name
                        << "))";
                }
            } else { // Between
                // TODO: Test which way is the fastest
                if (emptycheck) {
                    afl << "(merge(";
                    afl << "project(between(" << arr << "," << y_min << "," << x_min << ","
                        << y_max << "," << x_max << ")," << array.attrs[nband].name << ")";
                    afl << ",between(build(<" << array.attrs[nband].name << ":"
                        << array.attrs[nband].typeId << ((array.attrs[nband].nullable) ? " NULL" : " NOT NULL")
                        << "> [" << array.getYDim()->name
                        << "=" << array.getYDim()->start << ":"
                        << array.getYDim()->start + array.getYDim()->length - 1 << ","
                        << array.getYDim()->chunksize << "," << 0 << ","
                        << array.getXDim()->name << "=" << array.getXDim()->start << ":"
                        << array.getXDim()->start + array.getXDim()->length - 1 << ","
                        << array.getXDim()->chunksize << "," << 0 << "]," << naval << "),"
                        << y_min << "," << x_min << "," << y_max << "," << x_max << ")))";
                }

                else {
                    afl << "(project(between(" << arr << "," << y_min << "," << x_min << ","
                        << y_max << "," << x_max << ")," << array.attrs[nband].name << "))";
                }
            }
        }

        else {
            if (use_subarray) {
                if (emptycheck) {
                    afl << "transpose(merge(";
                    afl << "project(subarray(" << arr << "," << x_min << "," << y_min << ","
                        << x_max << "," << y_max << ")," << array.attrs[nband].name << ")";
                    afl << ",build(<" << array.attrs[nband].name << ":"
                        << array.attrs[nband].typeId << ((array.attrs[nband].nullable) ? " NULL" : " NOT NULL")                
                        << "> [" << array.getXDim()->name
                        << "=" << 0 << ":" << x_max - x_min << ","
                        << array.getXDim()->chunksize << "," << 0 << ","
                        << array.getYDim()->name << "=" << 0 << ":" << y_max - y_min << ","
                        << array.getYDim()->chunksize << "," << 0 << "],"
                        << naval << ")))";
                } else {
                    afl << "transpose(project(subarray(" << arr << "," << x_min << ","
                        << y_min << "," << x_max << "," << y_max << "),"
                        << array.attrs[nband].name << "))";
                }
            } else {
                if (emptycheck) {
                    afl << "transpose(merge(";
                    afl << "project(between(" << arr << "," << x_min << "," << y_min << ","
                        << x_max << "," << y_max << ")," << array.attrs[nband].name << ")";
                    afl << ",between(build(<" << array.attrs[nband].name << ":"
                        << array.attrs[nband].typeId << ((array.attrs[nband].nullable) ? " NULL" : " NOT NULL")
                        << "> [" << array.getXDim()->name
                        << "=" << array.getXDim()->start << ":"
                        << array.getXDim()->start + array.getXDim()->length - 1 << ","
                        << array.getXDim()->chunksize << "," << 0 << ","
                        << array.getYDim()->name << "=" << array.getYDim()->start << ":"
                        << array.getYDim()->start + array.getYDim()->length - 1 << ","
                        << array.getYDim()->chunksize << "," << 0 << "]," << naval << "),"
                        << x_min << "," << y_min << "," << x_max << "," << y_max << ")))";
                } else {
                    afl << "transpose(project(between(" << arr << "," << x_min << ","
                        << y_min << "," << x_max << "," << y_max << "),"
                        << array.attrs[nband].name << "))";
                }
            }
        }

        //  If attribute is nullable, apply substitute to fill null cells with default null value
        if (array.attrs[nband].nullable) {
            string afl_temp = afl.str();
            afl.str("");
            afl << "substitute(" <<  afl_temp <<  ", build(<val:" << array.attrs[nband].typeId <<  ">[i=0:0, 1, 0], " << naval  <<  "))";

        }
        
        
        
        Utils::debug("Performing AFL Query: " + afl.str());


        ss << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0)
           << "&save=" << "(" << array.attrs[nband].typeId;
        //if (array.attrs[nband].nullable) ss  << " " <<  "null";
        ss << ")";
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;

        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        response = "";
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                        &responseToStringCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
        curlPerform();
        curlEnd();

        curlBegin();
        // READ BYTES  ////////////////////////////
        ss.str("");
        ss << _host << SHIMENDPOINT_READ_BYTES << "?"
        << "id=" << sessionID << "&n=0";
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

        response = "";
        struct SingleAttributeChunk data;
        data.memory = (char*)outchunk;
        data.size = 0;

        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION, responseBinaryCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, (void*)&data);
        curlPerform();
        curlEnd();

        releaseSession(sessionID);

        outchunk = (void*)data.memory;

        return SUCCESS;
    }

    StatusCode ShimClient::createTempArray(SciDBSpatialArray& array) {
        if (array.name == "") {
            Utils::error("Cannot create unnamed arrays");
            return ERR_CREATE_INVALIDARRAYNAME;
        }
        bool exists;
        arrayExists(array.name, exists);
        if (exists) {
            if (!(_cp->type == ST_SERIES || _cp->type == ST_ARRAY ||
                _cp->type == S_ARRAY)) {
                Utils::error("Array '" + array.name +
                            "' already exists in SciDB database");
                return ERR_CREATE_ARRAYEXISTS;
            }
        }

        if (array.attrs.size() == 0) {
            Utils::error("No array attributes specified");
            return ERR_CREATE_UNKNOWN;
        }

        stringstream ss;

        int sessionID = newSession();

        // Build afl query, e.g. CREATE ARRAY A <x: double, err: double> [i=0:99,10,0,
        // j=0:99,10,0];
        stringstream afl;
        afl << "CREATE TEMP ARRAY " << array.name << SCIDB4GDAL_ARRAYSUFFIX_TEMP;
        
        afl <<  array.getSchemaString();
        
        
        // Append attribute specification
//         afl << " <";
//         for (uint32_t i = 0; i < array.attrs.size(); ++i) {
//             afl << array.attrs[i].name << ": " << array.attrs[i].typeId;
//             if (i != array.attrs.size() - 1) {
//                 afl << ",";
//             } else {
//                 afl << ">";
//             }
//         }
// 
//         // Append dimension spec
//         afl << " [";
//         for (uint32_t i = 0; i < array.dims.size(); ++i) {
//             string dimHigh = "";
//             if (array.dims[i].high == SCIDB_MAX_DIM_INDEX) {
//                 dimHigh = "*";
//             } else {
//                 dimHigh = boost::lexical_cast<string>(array.dims[i].high);
//             }
// 
//             // TODO: Overlap
//             afl << array.dims[i].name << "=" << array.dims[i].low << ":" << dimHigh
//                 << "," << array.dims[i].chunksize << "," << 0;
//             if (i != array.dims.size() - 1) {
//                 afl << ", ";
//             } else {
//                 afl << "]";
//             }
//         }


        string aflquery = afl.str();
        Utils::debug("Performing AFL Query: " + afl.str());

        // Utils::debug("AFL QUERY: " + afl.str());

        curlBegin();
        // EXECUTE QUERY  ////////////////////////////
        ss.str("");
        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
        << "id=" << sessionID
        << "&query=" << curl_easy_escape(_curl_handle, aflquery.c_str(), 0);
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;

        // Set the HTTP options URL and the operations
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        if (curlPerform() != CURLE_OK) {
            curlEnd();
            return ERR_CREATE_UNKNOWN;
        }
        curlEnd();

        releaseSession(sessionID);

        return SUCCESS;
    }

    StatusCode ShimClient::persistArray(string srcArr, string tarArr) {
        // create new array
        int sessionID = newSession();

        stringstream afl;
        afl << "store(" << srcArr << ", " << tarArr << ")";
        Utils::debug("Performing AFL Query: " + afl.str());

        curlBegin();
        // EXECUTE QUERY  ////////////////////////////
        stringstream ss;
        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
        << "id=" << sessionID
        << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0);
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth; // Add auth parameter if using ssl
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        if (curlPerform() != CURLE_OK) {
            curlEnd();
            return ERR_GLOBAL_UNKNOWN;
        }
        curlEnd();

        releaseSession(sessionID);

        return SUCCESS;
    }

    StatusCode ShimClient::insertInto(SciDBArray& srcArray, SciDBArray& destArray) {
        // create new array
        int sessionID = newSession();
        string collArr = destArray.name; // array name in which we want to store the data
        string tmpArr = srcArray.name;   // array name with the source data

        stringstream castSchema;
        castSchema << "<";

        // attributes
        for (uint32_t i = 0; i < srcArray.attrs.size(); i++) {
            string nullable = (srcArray.attrs[i].nullable) ? " NULL" : "";
            castSchema << srcArray.attrs[i].name << ":" << srcArray.attrs[i].typeId
                    << "" << nullable << ",";
        }

        // special handling for the over attributes (over_x, over_y, over_t) -> rename
        // them based on the stored names for dimensions in
        // the destination array
        if (SciDBSpatioTemporalArray* starray =
                dynamic_cast<SciDBSpatioTemporalArray*>(&destArray)) {
            // target attibute names from (over_x, over_y, over_t)
            castSchema << starray->getXDim()->name << ":int64 NULL, "
                    << starray->getYDim()->name << ":int64 NULL, "
                    << starray->getTDim()->name << ":int64 NULL>[";
        } else if (SciDBSpatialArray* sarray =
                    dynamic_cast<SciDBSpatialArray*>(&destArray)) {
            // eo_over creates attributes over_x,over_y AND over_t every time
            // target attibute names from (over_x, over_y, over_t)
            castSchema << sarray->getXDim()->name << ":int64 NULL, "
                    << sarray->getYDim()->name << ":int64 NULL, t:int64 NULL>[";
        } else {
            Utils::debug("Cannot cast array to spatial or spatiotemporal. Using "
                        "standard axis definitions.");
            castSchema << "x:int64 NULL, y:int64 NULL, t:int64 NULL>[";
        }

        // add dimension statement and rename it with src_{x|y|t}
        // [src_x=0:1000,512,0, ...] name=min:max,chunksize,overlap
        for (uint32_t i = 0; i < srcArray.dims.size(); i++) {
            string dim_name = srcArray.dims[i].name;
            string dimHigh;
            if (srcArray.dims[i].high == SCIDB_MAX_DIM_INDEX) {
                dimHigh = "*";
            } else {
                dimHigh = boost::lexical_cast<string>(srcArray.dims[i].high);
            }
            // TODO change statement for overlap
            castSchema << "src_" << dim_name << "=" << srcArray.dims[i].low << ":"
                    << dimHigh << "," << srcArray.dims[i].chunksize << ",0";

            if (i < srcArray.dims.size() - 1) {
                castSchema << ", ";
            }
        }

        castSchema << "]";
        // the dimensions of the src array will be discarded later

       
        
        
        string eo_over = "eo_over(" + tmpArr + "," + collArr + ")";
        string join = "join(" + tmpArr + ", " + eo_over + ")";
        string cast = "cast(" + join + ", " + castSchema.str() + ")";
        
        /* 2016-06-27: Added strict=false to ignore cell collisions  */
        string redimension = "redimension(" + cast + "," + collArr + ", false)";

        
        
        /* 2016-05-17: Moved filtering of NA values from insertData() to here  */
        
        stringstream afl_filternonNA, afl_predicateNA;

        // for each attribute get name and assigned NA value and add them to the
        // boolean statement (if NO_DATA was set)
        // check if NODATA value exists...
        unsigned int noNA = 0;
        for (uint32_t i = 0; i < srcArray.attrs.size(); ++i) {
            string naVal = srcArray.attrs[i].md[""]["NODATA"]; // if not exists then an empty string will be returned in this case
            if (naVal.empty()) {
                ++noNA;
                continue;
            }

            if (Utils::scidbTypeIdIsInteger(srcArray.attrs[i].typeId)) {
                long v = boost::lexical_cast<long>(naVal);
                afl_predicateNA<< srcArray.attrs[i].name << " = " << v;
            } else if (Utils::scidbTypeIdIsFloatingPoint(srcArray.attrs[i].typeId)) {
                double v = boost::lexical_cast<double>(naVal);
                afl_predicateNA << srcArray.attrs[i].name << " = " << std::setprecision(numeric_limits<double>::digits10) << v;
            } else continue;

            afl_predicateNA<< " OR ";
            
        }
        afl_predicateNA <<  " FALSE ";
        if (noNA < srcArray.attrs.size()) { // at least one nodata value was set
            afl_filternonNA << "filter(" << redimension << ", NOT (" << afl_predicateNA.str() << "))";
          
        } else {
            afl_filternonNA << redimension;
        }
        /* 2016-05-17: END */
        
        
    
        stringstream afl;
        afl << "insert(" << afl_filternonNA.str() << ", " << collArr << ")";
        Utils::debug("Performing AFL Query: " + afl.str());

        curlBegin();
        // EXECUTE QUERY  ////////////////////////////
        stringstream ss;
        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
        << "id=" << sessionID
        << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0);
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth; // Add auth parameter if using ssl
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        if (curlPerform() != CURLE_OK) {
            curlEnd();
            return ERR_GLOBAL_UNKNOWN;
        }
        curlEnd();

        releaseSession(sessionID);

        return SUCCESS;
    }

    /**
    * @brief Checks for illegal chars for file name use.
    *
    * This function is used to check bad letters for creating an appropriate file name for the temporary SciDB array. Allowed chars are alpha numeric values
    * and the following chars: '/', '_', '-' and '.'. All other chars are neglected.
    *
    * @param c char value to be tested
    */
    inline bool isIllegalFilenameCharacter(char c) {
        // This is probably incomplete but should be enough for shim generated
        // filenames on the server
        return !(std::isalnum(c) || c == '/' || c == '_' || c == '-' || c == '.');
    }

    StatusCode ShimClient::insertData(SciDBSpatialArray& array, void* inChunk,
                                    int32_t x_min, int32_t y_min, int32_t x_max,
                                    int32_t y_max) {
        // TODO: Do some checks

        // Shim create session
        int sessionID = newSession();

        // Shim upload file from binary stream
        string format = array.getFormatString();

        // Get total size in bytes of one pixel, i.e. sum of attribute sizes
        size_t pixelSize = 0;
        uint32_t nx = (1 + x_max - x_min);
        uint32_t ny = (1 + y_max - y_min);
        for (uint32_t i = 0; i < array.attrs.size(); ++i)
            pixelSize += Utils::scidbTypeIdBytes(array.attrs[i].typeId);
        size_t totalSize = pixelSize * nx * ny;

        Utils::debug("Upload file size " + boost::lexical_cast<string>(totalSize >> 10 >> 10) + "MB");

        // UPLOAD FILE ////////////////////////////
        stringstream ss;
        ss.str("");
    #ifdef CURL_ADDPORTTOURL
        ss << _host << SHIMENDPOINT_UPLOAD_FILE;
    #else
        ss << _host << ":" << _port << SHIMENDPOINT_UPLOAD_FILE;
    #endif
        ss << "?"
        << "id=" << sessionID;
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;

        struct curl_httppost* formpost = NULL;
        struct curl_httppost* lastptr = NULL;

        // Load file from buffer instead of file!
        // Form HTTP POST, first two pointers next the KVP for the form
        curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "file", CURLFORM_BUFFER,
                    SCIDB4GDAL_DEFAULT_UPLOAD_FILENAME, CURLFORM_BUFFERPTR, inChunk,
                    CURLFORM_BUFFERLENGTH, totalSize, CURLFORM_CONTENTTYPE,
                    "application/octet-stream", CURLFORM_END);

        curlBegin();
        string remoteFilename = "";
        // curl_easy_setopt(_curl_handle, CURLOPT_FOLLOWLOCATION, 1L);

        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPPOST, formpost);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION, &responseToStringCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &remoteFilename);

        if (curlPerform() != CURLE_OK) {
            curlEnd();
            return ERR_CREATE_UNKNOWN;
        }
        curlEnd();
        curl_formfree(formpost);

        // Remove special characters from remote filename
        remoteFilename.erase(std::remove_if(remoteFilename.begin(),
                                            remoteFilename.end(),
                                            isIllegalFilenameCharacter),
                            remoteFilename.end());

        // Load data from file to SciDB array
        stringstream afl_input;

        /** 2015-05-13: Use input instead of load **/
        SciDBSpatialArray array_tile = array;
        array_tile.getXDim()->start = x_min;
        array_tile.getXDim()->low = x_min;
        array_tile.getXDim()->high = x_max;
        array_tile.getXDim()->length = x_max - x_min + 1;

        array_tile.getYDim()->start = y_min;
        array_tile.getYDim()->low = y_min;
        array_tile.getYDim()->high = y_max;
        array_tile.getYDim()->length = y_max - y_min + 1;
        

        afl_input << "input(" << array_tile.getSchemaString() << ",'" << remoteFilename << "', -2, '" << format << "')";
        

        stringstream afl_redimension;
        if ((x_min == array.getXDim()->start) && (y_min == array.getYDim()->start) &&
            (x_max == array.getXDim()->start + array.getXDim()->length - 1) &&
            (y_max == array.getYDim()->start + array.getYDim()->length - 1)) {
            // repart() is much faster!
            afl_redimension << "repart(" << afl_input.str() << "," << array.getSchemaString() << ")";
        } else {
            afl_redimension << "redimension(" << afl_input.str() << "," << array.getSchemaString() << ")";
        }

        
        stringstream afl;
        afl << "insert(" << afl_redimension.str() << ", " << array.name << SCIDB4GDAL_ARRAYSUFFIX_TEMP << ")";
        Utils::debug("Performing AFL Query: " + afl.str());

        curlBegin();
        ss.str("");
        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
        << "id=" << sessionID
        << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0);
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        if (curlPerform() != CURLE_OK) {
            curlEnd();
            Utils::warn("Insertion or redimensioning of tile failed.");
            return ERR_CREATE_UNKNOWN;
        }
        curlEnd();

        // Release session
        releaseSession(sessionID);

        return SUCCESS;
    }

    StatusCode ShimClient::getAttributeStats(SciDBSpatialArray& array,
                                            uint8_t nband,
                                            SciDBAttributeStats& out) {
        if (nband >= array.attrs.size()) {
            Utils::error("Invalid attribute index");
        }

        int sessionID = newSession();

        stringstream ss, afl;
        string aname = array.attrs[nband].name;

        // create an output table based on the name of array
        // min(), max(),avg(),stdev()
        // and save it as 4 double values
        afl << "aggregate(" << array.name << ",min(" << aname << "),max(" << aname << "),avg(" << aname << "),stdev(" << aname << "))";
        Utils::debug("Performing AFL Query: " + afl.str());

        ss.str();
        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
        << "id=" << sessionID << "&query=" << afl.str() << "&save="
        << "(double,double,double,double)";
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;

        curlBegin();
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        curlPerform();
        curlEnd();

        curlBegin();
        // READ BYTES  ////////////////////////////
        ss.str("");
        ss << _host << SHIMENDPOINT_READ_BYTES << "?"
        << "id=" << sessionID << "&n=0";
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

        struct SingleAttributeChunk data;
        data.memory = (char*)malloc(sizeof(double) * 4);
        data.size = 0;

        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION, responseBinaryCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, (void*)&data);
        curlPerform();
        curlEnd();

        out.min = ((double*)data.memory)[0];
        out.max = ((double*)data.memory)[1];
        out.mean = ((double*)data.memory)[2];
        out.stdev = ((double*)data.memory)[3];

        free(data.memory);

        releaseSession(sessionID);

        return SUCCESS;
    }

    StatusCode ShimClient::arrayExists(const string& inArrayName, bool& out) {
        stringstream ss, afl;

        int sessionID = newSession();

        // There might be less complex queries but this one always succeeds and does
        // not give HTTP 500 SciDB errors
        afl << "aggregate(filter(list('arrays'),name='" << inArrayName
            << "'),count(name))";
        Utils::debug("Performing AFL Query: " + afl.str());

        // 	createSHIMExecuteString(ss, sessionID, afl);
        ss.str();
        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?" << "id=" << sessionID << "&query=" << afl.str() << "&save=" << "(int64)";
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;

        curlBegin();
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        curlPerform();
        curlEnd();

        curlBegin();
        // READ BYTES  ////////////////////////////
        ss.str("");
        ss << _host << SHIMENDPOINT_READ_BYTES << "?"
        << "id=" << sessionID << "&n=0";
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

        struct SingleAttributeChunk data;
        // Expect just one integer
        data.memory = (char*)malloc(sizeof(int64_t) * 1);
        data.size = 0;

        curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION, responseBinaryCallback);
        curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, (void*)&data);
        curlPerform();
        curlEnd();

        uint64_t count = ((int64_t*)data.memory)[0];

        if (count > 0)
            out = true;
        else
            out = false;

        free(data.memory);

        releaseSession(sessionID);

        return SUCCESS;
    }

    StatusCode ShimClient::updateSRS(SciDBSpatialArray& array) {
        
        if (!hasSCIDB4GEO()) {
          Utils::warn("SciDB server does not run spacetime extension. Update of spatial reference system skipped.");
          return SUCCESS;
        }
        
        // Add spatial reference system information if available
        if (array.srtext != "") {
            int sessionID = newSession();

            /* In the following, we assume the SRS to be already known by scidb4geo.
            This might only work for EPSG codes and in the future, this should be
            checked and
            unknown SRS should be registered via st_regnewsrs() automatically */
            curlBegin();
            // EXECUTE QUERY  ////////////////////////////
            stringstream afl;
            afl << "st_setsrs(" << array.name << ",'" << array.getXDim()->name << "','"
                << array.getYDim()->name << "','" << array.auth_name << "',"
                << array.auth_srid << ",'" << array.affineTransform.toString() << "')";
            Utils::debug("Performing AFL Query: " + afl.str());

            stringstream ss;
            ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
            << "id=" << sessionID
            << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0);
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
            curlPerform();
            curlEnd();

            releaseSession(sessionID);
        } else {
            // TODO: How to remove SRS information? Is this neccessary at all?
            Utils::debug("No spatial reference was set. Continuing without SR. Maybe no longer referenceable by GDAL");
        }

        return SUCCESS;
    }

    StatusCode ShimClient::removeArray(const string& inArrayName) {
        int sessionID = newSession();

        curlBegin();
        stringstream ss, afl;
        afl << "remove(" << inArrayName << ")";

        Utils::debug("Performing AFL Query: " + afl.str());

        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
        << "id=" << sessionID << "&query=" << afl.str();
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth; // Add auth parameter if using ssl

        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        if (curlPerform() != CURLE_OK) {
            curlEnd();
            Utils::warn("Cannot remove array '" + inArrayName + "'");
            return ERR_GLOBAL_UNKNOWN;
        }
        curlEnd();
        releaseSession(sessionID);
        return SUCCESS;
    }

    StatusCode ShimClient::setArrayMD(string arrayname,
                                    std::map<std::string, std::string> kv,
                                    string domain) {
                                        
        if (!hasSCIDB4GEO()) {
            Utils::warn("SciDB server does not run spacetime extension. Update of array metadata skipped.");
            return SUCCESS;                               
        }
        stringstream key_array_str;
        stringstream val_array_str;

        uint32_t i = 0;
        for (std::map<string, string>::iterator it = kv.begin(); it != kv.end();
            ++it) {
            key_array_str << it->first;
            val_array_str << it->second;
            if (i++ < kv.size() - 1) {
                val_array_str << ",";
                key_array_str << ",";
            }
        }

        int sessionID = newSession();

        curlBegin();
        stringstream ss, afl;
        afl << "eo_setmd(" << arrayname << ",'" << key_array_str.str() << "','"
            << val_array_str.str() << "')";
        Utils::debug("Performing AFL Query: " + afl.str());
        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
        << "id=" << sessionID
        << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0);
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        if (curlPerform() != CURLE_OK) {
            curlEnd();
            Utils::warn("Cannot set metadata for array '" + arrayname + "'");
            return ERR_GLOBAL_UNKNOWN;
        }
        curlEnd();
        releaseSession(sessionID);
        return SUCCESS;
    }

    StatusCode ShimClient::getArrayMD(std::map<string, string>& kv,
                                    string arrayname, string domain) {
        if (!hasSCIDB4GEO()) {
            Utils::warn("SciDB server does not run spacetime extension. Reading array metadata skipped.");
            return SUCCESS;                               
        }
        int sessionID = newSession();
        stringstream ss;
        string response;
        {
            curlBegin();
            ss.str("");
            stringstream afl;
            afl.str("");
            afl << "project(filter(eo_getmd(" << arrayname
                << "),attribute='' and domain='" << domain << "'), key, value)";

            Utils::debug("Performing AFL Query: " + afl.str());

            ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
            << "id=" << sessionID
            << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0)
            << "&save="
            << "csv";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::error("Cannot get metadata for array '" + arrayname + "'.");
                return ERR_READ_UNKNOWN;
            }
            curlEnd();
        }

        /**
        * Read data from the request and save it at the variable "response"
        */
        {
            curlBegin();
            ss.str("");
            // READ BYTES  ///////////////////////////
            ss << _host << SHIMENDPOINT_READ_BYTES << "?"
            << "id=" << sessionID << "&n=0";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::error("Cannot get metadata for array '" + arrayname + "'.");
                return ERR_READ_UNKNOWN;
            }
            curlEnd();
        }

    
        CSVstring* csv;
        if (!isVersionGreaterThan(15,7)) {
         csv = new CSVstring( response,true); // with header  
        }
        else csv = new CSVstring( response,false); // with header  
        
        if (csv->nrow() == 0) {
            Utils::debug("Array '" + arrayname + "' has no additional metadata, skipping.");
            delete csv;
            releaseSession(sessionID);
            return SUCCESS;
        }
        
        if ( csv->ncol() != 2) 
        {
            delete csv;
            Utils::error("Cannot extract metadata of array '" + arrayname + "'.");
            releaseSession(sessionID);
            return ERR_GLOBAL_PARSE;
        }
        
        for (int i=0; i<csv->nrow(); ++i)
        {
            string key = csv->get<string>(i,0).substr(1, csv->get<string>(i,0).length() - 2);
            string val = csv->get<string>(i,1).substr(1, csv->get<string>(i,1).length() - 2);
            kv.insert(std::pair<string, string>(key, val));        
        }
        
        delete csv;
        releaseSession(sessionID);

        return SUCCESS;
    }

    StatusCode ShimClient::setAttributeMD(string arrayname, string attribute,
                                        map<string, string> kv, string domain) {
        
        if (!hasSCIDB4GEO()) {
            Utils::warn("SciDB server does not run spacetime extension. Update of attribute metadata skipped.");
            return SUCCESS;                               
        }
        stringstream key_array_str;
        stringstream val_array_str;

        uint32_t i = 0;
        for (std::map<string, string>::iterator it = kv.begin(); it != kv.end();
            ++it) {
            key_array_str << it->first;
            val_array_str << it->second;
            if (i++ < kv.size() - 1) {
                val_array_str << ",";
                key_array_str << ",";
            }
        }

        int sessionID = newSession();

        curlBegin();
        stringstream ss, afl;
        afl << "eo_setmd(" << arrayname << ",'" << attribute << "','"
            << key_array_str.str() << "','" << val_array_str.str() << "')";
        Utils::debug("Performing AFL Query: " + afl.str());
        ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
        << "id=" << sessionID
        << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0);
        // Add auth parameter if using ssl
        if (_ssl && !_auth.empty())
            ss << "&auth=" << _auth;
        curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
        curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
        if (curlPerform() != CURLE_OK) {
            curlEnd();
            Utils::warn("Cannot set metadata for attribute '" + arrayname + "." +
                        attribute + "'");
            return ERR_GLOBAL_UNKNOWN;
        }
        curlEnd();
        releaseSession(sessionID);
        return SUCCESS;
    }

    StatusCode ShimClient::getAttributeMD(std::map<std::string, std::string>& kv,
                                        std::string arrayname, string attribute,
                                        std::string domain) {
        if (!hasSCIDB4GEO()) {
            Utils::warn("SciDB server does not run spacetime extension. Reading attribute metadata skipped.");
            return SUCCESS;                               
        }
        int sessionID = newSession();
        stringstream ss;
        string response;
        {
            curlBegin();
            stringstream afl;
            afl << "project(filter(eo_getmd(" << arrayname << "),attribute='"
                << attribute << "' and domain='" << domain << "'), key, value)";

            Utils::debug("Performing AFL Query: " + afl.str());

            ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
            << "id=" << sessionID
            << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0)
            << "&save="
            << "csv";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::error("Cannot get metadata for attribute '" + arrayname + "." +
                            attribute + "'.");
                return ERR_READ_UNKNOWN;
            }
            curlEnd();
        }

        /**
    * Read data from the request and save it at the variable "response"
    */
        {
            curlBegin();
            ss.str("");
            // READ BYTES  ///////////////////////////
            ss << _host << SHIMENDPOINT_READ_BYTES << "?"
            << "id=" << sessionID << "&n=0";
            // Add auth parameter if using ssl
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth;
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);

            response = "";
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION,
                            &responseToStringCallback);
            curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, &response);
            if (curlPerform() != CURLE_OK) {
                curlEnd();
                Utils::error("Cannot get metadata for array '" + arrayname + "'.");
                return ERR_READ_UNKNOWN;
            }
            curlEnd();
        }

        
        CSVstring* csv;
        if (!isVersionGreaterThan(15,7)) {
         csv = new CSVstring( response,true); // with header  
        }
        else csv = new CSVstring( response,false); // with header  
        if (csv->nrow() == 0) {
            Utils::debug("Array attribute '" + attribute + "' has no additional metadata, skipping.");
            delete csv;
            releaseSession(sessionID);
            return SUCCESS;
        }
        if ( csv->ncol() != 2) 
        {
            delete csv;
            Utils::warn("Cannot extract metadata of array '" + arrayname + "'.");
            releaseSession(sessionID);
            return ERR_GLOBAL_PARSE;
        }
        
        for (int i=0; i<csv->nrow(); ++i)
        {
            string key = csv->get<string>(i,0).substr(1, csv->get<string>(i,0).length() - 2);
            string val = csv->get<string>(i,1).substr(1, csv->get<string>(i,1).length() - 2);
            kv.insert(std::pair<string, string>(key, val));        
        }
        
        delete csv;


        releaseSession(sessionID);

        return SUCCESS;
    }

    void ShimClient::setCreateParameters(CreationParameters& par) { _cp = &par; }

    void ShimClient::setConnectionParameters(ConnectionParameters& par) {
        _conp = &par;
    }

    void ShimClient::setQueryParameters(QueryParameters& par) { _qp = &par; }

    StatusCode ShimClient::updateTRS(SciDBTemporalArray& array) {
        if (!hasSCIDB4GEO()) {
          Utils::warn("SciDB server does not run spacetime extension. Update of temporal reference system skipped.");
          return SUCCESS;
        }
        // Add temporal reference system information if available
        if (array.getTPoint() != NULL && array.getTInterval() != NULL &&
            array.getTInterval()->toStringISO() != "P") {
            int sessionID = newSession();

            curlBegin();
            // EXECUTE QUERY  ////////////////////////////
            stringstream afl;
            afl << "st_settrs(" << array.name << ",'" << array.tdim << "','"
                << array.getTPoint()->toStringISO() << "','"
                << array.getTInterval()->toStringISO() << "')";
            Utils::debug("Performing AFL Query: " + afl.str());

            stringstream ss;
            ss << _host << SHIMENDPOINT_EXECUTEQUERY << "?"
            << "id=" << sessionID
            << "&query=" << curl_easy_escape(_curl_handle, afl.str().c_str(), 0);
            if (_ssl && !_auth.empty())
                ss << "&auth=" << _auth; // Add auth parameter if using ssl
            curl_easy_setopt(_curl_handle, CURLOPT_URL, ss.str().c_str());
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPGET, 1);
            curlPerform();
            curlEnd();

            releaseSession(sessionID);
        } else {
            // TODO: How to remove SRS information? Is this neccessary at all?
            return ERR_TRS_INVALID;
        }

        return SUCCESS;
    }
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    /****************************************************************************************** */
    
    
    CSVstring::CSVstring(const string& s) : 
         _s(s),
        _colsep(","),
        _rowsep("\n"),
        _header(false),
        _cells(NULL),
        _head(NULL),
        _ncol(-1),
        _nrow(-1) { }

    
    
    CSVstring::CSVstring(const string& s, bool header) : 
        _s(s),
        _colsep(","),
        _rowsep("\n"),
        _header(header),
        _cells(NULL),
        _head(NULL),
        _ncol(-1),
        _nrow(-1) { } 

    
    CSVstring::CSVstring(const string& s, const string& colsep, const string& rowsep, bool header) :
        _s(s),
        _colsep(colsep),
        _rowsep(rowsep),
        _header(header),
        _cells(NULL),
        _head(NULL),
        _ncol(-1),
        _nrow(-1) { }

        
    CSVstring::~CSVstring()
    {
        if (_cells != NULL) 
        {
            delete _cells;
            _cells = NULL;
        }
        if (_head != NULL) 
        {
            delete _head;
            _head = NULL;
        }
    }
    
    template<typename T> T CSVstring::get(int row, int col)
    {
        if (_cells == NULL) process();
        
        if (row >= _cells->size() || row < 0) {
            Utils::error("Invalid CSV row requested!");
        }
        if (col >= (*_cells)[row].size() || col < 0) {
            Utils::error("Invalid CSV col requested!");
        }
        return boost::lexical_cast<T>((*_cells)[row][col]);
        
    }
    
    int CSVstring::ncol()
    {
        if (_cells == NULL) process();
        return _ncol;
    }


    int CSVstring::nrow()
    {
        if (_cells == NULL) process();
        return _nrow;
    }
    
    void CSVstring::process()
    {
        if (_cells != NULL) {
            delete _cells;
            _cells = NULL;
        }
         _nrow = 0;
         _ncol = 0;
        _cells = new vector<vector<string> >;

        
        vector<string> rows = Utils::split(_s, _rowsep);
       
        
        size_t i=0;
        if (_header) {
            while (rows[i].empty() && i<rows.size()) ++i;
            vector <string> cols = Utils::split(rows[i],_colsep);
            _head = new vector<string>(cols);
            ++i;
        }
        

        while( i<rows.size()) 
        {
            if (rows[i].empty()) { 
                ++i;
                continue;
            }
            
            vector <string> cols = Utils::split(rows[i],_colsep);
            if (_ncol <= 0) _ncol = cols.size();
            if (_ncol != cols.size())
            {
                stringstream ss;
                ss << "Unexpected number of colums in CSV string at line  " << i+1 << ": expected " << _ncol << " but had " << cols.size(); 
                Utils::warn(ss.str());
            }
            
            ++_nrow;
            _cells->push_back(cols);
            ++i;
        }
    }

    
    
    
    
    
    
    
}
