#include "AsyncDownloader.h"
#include "Platform.h"
#include "Funcs.h"
#include "ErrorHandler.h"
#include "Stream.h"
#include "GameStructs.h"

void ASyncRequest_Free(struct AsyncRequest* request) {
	Mem_Free(request->ResultData);
	request->ResultData = NULL;
	request->ResultSize = 0;
}

#define ASYNCREQUESTLIST_DEFELEMS 10
struct AsyncRequestList {
	Int32 MaxElems, Count;
	struct AsyncRequest* Requests;
	struct AsyncRequest DefaultRequests[ASYNCREQUESTLIST_DEFELEMS];
};

static void AsyncRequestList_EnsureSpace(struct AsyncRequestList* list) {
	if (list->Count < list->MaxElems) return;
	Utils_Resize(&list->Requests, &list->MaxElems, sizeof(struct AsyncRequest),
		ASYNCREQUESTLIST_DEFELEMS, 10);
}

static void AsyncRequestList_Append(struct AsyncRequestList* list, struct AsyncRequest* item) {
	AsyncRequestList_EnsureSpace(list);
	list->Requests[list->Count++] = *item;
}

static void AsyncRequestList_Prepend(struct AsyncRequestList* list, struct AsyncRequest* item) {
	AsyncRequestList_EnsureSpace(list);
	Int32 i;
	for (i = list->Count; i > 0; i--) {
		list->Requests[i] = list->Requests[i - 1];
	}
	list->Requests[0] = *item;
	list->Count++;
}

static void AsyncRequestList_RemoveAt(struct AsyncRequestList* list, Int32 i) {
	if (i < 0 || i >= list->Count) ErrorHandler_Fail("Tried to remove element at list end");

	for (; i < list->Count - 1; i++) {
		list->Requests[i] = list->Requests[i + 1];
	}
	list->Count--;
}

static void AsyncRequestList_Init(struct AsyncRequestList* list) {
	list->MaxElems = ASYNCREQUESTLIST_DEFELEMS;
	list->Count = 0;
	list->Requests = list->DefaultRequests;
}

static void AsyncRequestList_Free(struct AsyncRequestList* list) {
	if (list->Requests != list->DefaultRequests) {
		Mem_Free(list->Requests);
	}
	AsyncRequestList_Init(list);
}

void* async_waitable;
void* async_workerThread;
void* async_pendingMutex;
void* async_processedMutex;
void* async_curRequestMutex;
volatile bool async_terminate;

struct AsyncRequestList async_pending;
struct AsyncRequestList async_processed;
String async_skinServer = String_FromConst("http://static.classicube.net/skins/");
struct AsyncRequest async_curRequest;
volatile Int32 async_curProgress = ASYNC_PROGRESS_NOTHING;
/* TODO: Implement these */
bool ManageCookies;
bool KeepAlive;
/* TODO: Connection pooling */

static void AsyncDownloader_Add(String* url, bool priority, String* id, UInt8 type, UInt64* lastModified, String* etag, String* data) {
	Mutex_Lock(async_pendingMutex);
	{
		struct AsyncRequest req = { 0 };
		String reqUrl = String_FromArray(req.URL); String_Copy(&reqUrl, url);
		String reqID  = String_FromArray(req.ID);  String_Copy(&reqID, id);
		req.RequestType = type;

		Platform_Log2("Adding %s (type %b)", &reqUrl, &type);

		if (lastModified) {
			req.LastModified = *lastModified;
		}
		if (etag) {
			String reqEtag = String_FromArray(req.Etag); String_Copy(&reqEtag, etag);
		}
		/* request.Data = data; TODO: Implement this. do we need to copy or expect caller to malloc it?  */

		req.TimeAdded = DateTime_CurrentUTC_MS();
		if (priority) {
			AsyncRequestList_Prepend(&async_pending, &req);
		} else {
			AsyncRequestList_Append(&async_pending, &req);
		}
	}
	Mutex_Unlock(async_pendingMutex);
	Waitable_Signal(async_waitable);
}

void AsyncDownloader_GetSkin(STRING_PURE String* id, STRING_PURE String* skinName) {
	char urlBuffer[STRING_SIZE];
	String url = String_FromArray(urlBuffer);

	if (Utils_IsUrlPrefix(skinName, 0)) {
		String_Copy(&url, skinName);
	} else {
		String_AppendString(&url, &async_skinServer);
		String_AppendColorless(&url, skinName);
		String_AppendConst(&url, ".png");
	}

	AsyncDownloader_Add(&url, false, id, REQUEST_TYPE_DATA, NULL, NULL, NULL);
}

void AsyncDownloader_GetData(STRING_PURE String* url, bool priority, STRING_PURE String* id) {
	AsyncDownloader_Add(url, priority, id, REQUEST_TYPE_DATA, NULL, NULL, NULL);
}

void AsyncDownloader_GetContentLength(STRING_PURE String* url, bool priority, STRING_PURE String* id) {
	AsyncDownloader_Add(url, priority, id, REQUEST_TYPE_CONTENT_LENGTH, NULL, NULL, NULL);
}

void AsyncDownloader_PostString(STRING_PURE String* url, bool priority, STRING_PURE String* id, STRING_PURE String* contents) {
	AsyncDownloader_Add(url, priority, id, REQUEST_TYPE_DATA, NULL, NULL, contents);
}

void AsyncDownloader_GetDataEx(STRING_PURE String* url, bool priority, STRING_PURE String* id, UInt64* lastModified, STRING_PURE String* etag) {
	AsyncDownloader_Add(url, priority, id, REQUEST_TYPE_DATA, lastModified, etag, NULL);
}

void AsyncDownloader_PurgeOldEntriesTask(struct ScheduledTask* task) {
	Mutex_Lock(async_processedMutex);
	{
		UInt64 now = DateTime_CurrentUTC_MS();
		Int32 i;

		for (i = async_processed.Count - 1; i >= 0; i--) {
			struct AsyncRequest* item = &async_processed.Requests[i];
			if (item->TimeDownloaded + (10 * 1000) >= now) continue;

			ASyncRequest_Free(item);
			AsyncRequestList_RemoveAt(&async_processed, i);
		}
	}
	Mutex_Unlock(async_processedMutex);
}

static Int32 AsyncRequestList_Find(STRING_PURE String* id, struct AsyncRequest* item) {
	Int32 i;
	for (i = 0; i < async_processed.Count; i++) {
		String reqID = String_FromRawArray(async_processed.Requests[i].ID);
		if (!String_Equals(&reqID, id)) continue;

		*item = async_processed.Requests[i];
		return i;
	}
	return -1;
}

bool AsyncDownloader_Get(STRING_PURE String* id, struct AsyncRequest* item) {
	bool success = false;

	Mutex_Lock(async_processedMutex);
	{
		Int32 i = AsyncRequestList_Find(id, item);
		success = i >= 0;
		if (success) AsyncRequestList_RemoveAt(&async_processed, i);
	}
	Mutex_Unlock(async_processedMutex);
	return success;
}

bool AsyncDownloader_GetCurrent(struct AsyncRequest* request, Int32* progress) {
	Mutex_Lock(async_curRequestMutex);
	{
		*request   = async_curRequest;
		*progress = async_curProgress;
	}
	Mutex_Unlock(async_curRequestMutex);
	return request->ID[0];
}

static void AsyncDownloader_ProcessRequest(struct AsyncRequest* request) {
	String url = String_FromRawArray(request->URL);
	Platform_Log2("Downloading from %s (type %b)", &url, &request->RequestType);
	UInt64 timer;

	Stopwatch_Measure(&timer);
	request->Result = Http_Do(request, &async_curProgress);
	UInt32 elapsed  = Stopwatch_ElapsedMicroseconds(&timer) / 1000;

	Int32 status = request->StatusCode;
	Platform_Log3("HTTP: return code %i (http %i), in %i ms", 
		&request->Result, &status, &elapsed);

	if (request->ResultData) {
		UInt32 size = request->ResultSize;
		UInt64 addr = (UInt64)request->ResultData;
		Platform_Log2("HTTP returned data: %i bytes at %x", &size, &addr);
	}
}

static void AsyncDownloader_CompleteResult(struct AsyncRequest* request) {
	request->TimeDownloaded = DateTime_CurrentUTC_MS();
	Mutex_Lock(async_processedMutex);
	{
		struct AsyncRequest older;
		String id = String_FromRawArray(request->ID);
		Int32 index = AsyncRequestList_Find(&id, &older);

		if (index >= 0) {
			/* very rare case - priority item was inserted, then inserted again (so put before first item), */
			/* and both items got downloaded before an external function removed them from the queue */
			if (older.TimeAdded > request->TimeAdded) {
				struct AsyncRequest tmp = older; older = *request; *request = tmp;
			}

			ASyncRequest_Free(&older);
			async_processed.Requests[index] = *request;
		} else {
			AsyncRequestList_Append(&async_processed, request);
		}
	}
	Mutex_Unlock(async_processedMutex);
}

static void AsyncDownloader_WorkerFunc(void) {
	while (true) {
		struct AsyncRequest request;
		bool hasRequest = false;

		Mutex_Lock(async_pendingMutex);
		{
			if (async_terminate) return;
			if (async_pending.Count) {
				request = async_pending.Requests[0];
				hasRequest = true;
				AsyncRequestList_RemoveAt(&async_pending, 0);
			}
		}
		Mutex_Unlock(async_pendingMutex);

		if (hasRequest) {
			Platform_LogConst("Got something to do!");
			Mutex_Lock(async_curRequestMutex);
			{
				async_curRequest = request;
				async_curProgress = ASYNC_PROGRESS_MAKING_REQUEST;
			}
			Mutex_Unlock(async_curRequestMutex);

			Platform_LogConst("Doing it");
			AsyncDownloader_ProcessRequest(&request);
			AsyncDownloader_CompleteResult(&request);

			Mutex_Lock(async_curRequestMutex);
			{
				async_curRequest.ID[0] = '\0';
				async_curProgress = ASYNC_PROGRESS_NOTHING;
			}
			Mutex_Unlock(async_curRequestMutex);
		} else {
			Platform_LogConst("Going back to sleep...");
			Waitable_Wait(async_waitable);
		}
	}
}


static void AsyncDownloader_Init(void) {
	AsyncRequestList_Init(&async_pending);
	AsyncRequestList_Init(&async_processed);
	Http_Init();

	async_waitable = Waitable_Create();
	async_pendingMutex = Mutex_Create();
	async_processedMutex = Mutex_Create();
	async_curRequestMutex = Mutex_Create();
	async_workerThread = Thread_Start(AsyncDownloader_WorkerFunc);
}

static void AsyncDownloader_Reset(void) {
	Mutex_Lock(async_pendingMutex);
	{
		AsyncRequestList_Free(&async_pending);
	}
	Mutex_Unlock(async_pendingMutex);
	Waitable_Signal(async_waitable);
}

static void AsyncDownloader_Free(void) {
	async_terminate = true;
	AsyncDownloader_Reset();
	Thread_Join(async_workerThread);
	Thread_FreeHandle(async_workerThread);

	AsyncRequestList_Free(&async_pending);
	AsyncRequestList_Free(&async_processed);
	Http_Free();

	Waitable_Free(async_waitable);
	Mutex_Free(async_pendingMutex);
	Mutex_Free(async_processedMutex);
	Mutex_Free(async_curRequestMutex);
}

void AsyncDownloader_MakeComponent(struct IGameComponent* comp) {
	comp->Init  = AsyncDownloader_Init;
	comp->Reset = AsyncDownloader_Reset;
	comp->Free  = AsyncDownloader_Free;
}
