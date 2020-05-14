// simplewall
// Copyright (c) 2016-2020 Henry++

#include "global.hpp"

bool _wfp_isfiltersapplying ()
{
	return _r_fastlock_islocked (&lock_apply) || _r_fastlock_islocked (&lock_transaction);
}

EnumInstall _wfp_isproviderinstalled (HANDLE hengine)
{
	// check for persistent provider
	HKEY hkey;

	if (RegOpenKeyEx (HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\services\\BFE\\Parameters\\Policy\\Persistent\\Provider", 0, KEY_READ, &hkey) == ERROR_SUCCESS)
	{
		static const rstring guidString = _r_str_fromguid (GUID_WfpProvider);

		if (RegQueryValueEx (hkey, guidString.GetString (), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
		{
			RegCloseKey (hkey);
			return InstallEnabled;
		}

		RegCloseKey (hkey);
	}

	// check for temporary provider
	FWPM_PROVIDER *ptr_provider;

	if (FwpmProviderGetByKey (hengine, &GUID_WfpProvider, &ptr_provider) == ERROR_SUCCESS)
	{
		FwpmFreeMemory ((void**)&ptr_provider);

		return InstallEnabledTemporary;
	}

	return InstallDisabled;
}

EnumInstall _wfp_issublayerinstalled (HANDLE hengine)
{
	// check for persistent sublayer
	HKEY hkey;

	if (RegOpenKeyEx (HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\services\\BFE\\Parameters\\Policy\\Persistent\\SubLayer", 0, KEY_READ, &hkey) == ERROR_SUCCESS)
	{
		static const rstring guidString = _r_str_fromguid (GUID_WfpSublayer);

		if (RegQueryValueEx (hkey, guidString.GetString (), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
		{
			RegCloseKey (hkey);
			return InstallEnabled;
		}

		RegCloseKey (hkey);
	}

	// check for temporary sublayer
	FWPM_SUBLAYER *ptr_sublayer;

	if (FwpmSubLayerGetByKey (hengine, &GUID_WfpSublayer, &ptr_sublayer) == ERROR_SUCCESS)
	{
		FwpmFreeMemory ((void**)&ptr_sublayer);

		return InstallEnabledTemporary;
	}

	return InstallDisabled;
}

EnumInstall _wfp_isfiltersinstalled ()
{
	HANDLE hengine = _wfp_getenginehandle ();

	if (hengine)
		return _wfp_isproviderinstalled (hengine);

	return InstallDisabled;
}

HANDLE _wfp_getenginehandle ()
{
	if (_r_fs_isvalidhandle (config.hengine))
		return config.hengine;

	RtlSecureZeroMemory (&session, sizeof (session));

	session.displayData.name = APP_NAME;
	session.displayData.description = APP_NAME;

	session.txnWaitTimeoutInMSec = TRANSACTION_TIMEOUT;

	DWORD rc = FwpmEngineOpen (nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &config.hengine);

	if (rc != ERROR_SUCCESS)
	{
		app.LogError (L"FwpmEngineOpen", rc, nullptr, UID);
		config.hengine = nullptr;
	}

	return config.hengine;
}

bool _wfp_initialize (HANDLE hengine, bool is_full)
{
	bool result;
	DWORD rc;

	_r_fastlock_acquireshared (&lock_transaction);

	if (hengine)
	{
		result = true; // already initialized
	}
	else
	{
		hengine = _wfp_getenginehandle ();

		if (!hengine)
		{
			result = false;

			goto DoExit;
		}

		result = true;
	}

	_app_setsecurityinfoforengine (hengine);

	// install engine provider and it's sublayer
	bool is_providerexist = (_wfp_isproviderinstalled (hengine) != InstallDisabled);
	bool is_sublayerexist = (_wfp_issublayerinstalled (hengine) != InstallDisabled);

	if (is_full)
	{
		if (!is_providerexist || !is_sublayerexist)
		{
			bool is_intransact = _wfp_transact_start (hengine, __LINE__);

			if (!is_providerexist)
			{
				// create provider
				FWPM_PROVIDER provider = {0};

				provider.displayData.name = APP_NAME;
				provider.displayData.description = APP_NAME;

				provider.providerKey = GUID_WfpProvider;

				if (!config.is_filterstemporary)
					provider.flags = FWPM_PROVIDER_FLAG_PERSISTENT;

				rc = FwpmProviderAdd (hengine, &provider, nullptr);

				if (rc != ERROR_SUCCESS && rc != FWP_E_ALREADY_EXISTS)
				{
					if (is_intransact)
					{
						FwpmTransactionAbort (hengine);
						is_intransact = false;
					}

					app.LogError (L"FwpmProviderAdd", rc, nullptr, UID);
					result = false;

					goto DoExit;
				}
				else
				{
					is_providerexist = true;
				}
			}

			if (!is_sublayerexist)
			{
				FWPM_SUBLAYER sublayer = {0};

				sublayer.displayData.name = APP_NAME;
				sublayer.displayData.description = APP_NAME;

				sublayer.providerKey = const_cast<LPGUID>(&GUID_WfpProvider);
				sublayer.subLayerKey = GUID_WfpSublayer;
				sublayer.weight = (UINT16)app.ConfigGet (L"SublayerWeight", SUBLAYER_WEIGHT_DEFAULT).AsUint (); // highest weight for UINT16

				if (!config.is_filterstemporary)
					sublayer.flags = FWPM_SUBLAYER_FLAG_PERSISTENT;

				rc = FwpmSubLayerAdd (hengine, &sublayer, nullptr);

				if (rc != ERROR_SUCCESS && rc != FWP_E_ALREADY_EXISTS)
				{
					if (is_intransact)
					{
						FwpmTransactionAbort (hengine);
						is_intransact = false;
					}

					app.LogError (L"FwpmSubLayerAdd", rc, nullptr, UID);
					result = false;

					goto DoExit;
				}
				else
				{
					is_sublayerexist = true;
				}
			}

			if (is_intransact)
			{
				if (!_wfp_transact_commit (hengine, __LINE__))
					result = false;
			}
		}
	}

	// set security information
	if (is_providerexist || is_sublayerexist)
	{
		const bool is_secure = app.ConfigGet (L"IsSecureFilters", true).AsBool ();

		_app_setsecurityinfoforprovider (hengine, &GUID_WfpProvider, is_secure);
		_app_setsecurityinfoforsublayer (hengine, &GUID_WfpSublayer, is_secure);
	}

	// set engine options
	if (is_full)
	{
		static const bool is_win8 = _r_sys_validversion (6, 2);

		FWP_VALUE val;

		// dropped packets logging (win7+)
		if (!config.is_neteventset)
		{
			val.type = FWP_UINT32;
			val.uint32 = 1;

			rc = FwpmEngineSetOption (hengine, FWPM_ENGINE_COLLECT_NET_EVENTS, &val);

			if (rc != ERROR_SUCCESS)
			{
				app.LogError (L"FwpmEngineSetOption", rc, L"FWPM_ENGINE_COLLECT_NET_EVENTS", 0);
			}
			else
			{
				// configure dropped packets logging (win8+)
				if (is_win8)
				{
					// the filter engine will collect wfp network events that match any supplied key words
					val.type = FWP_UINT32;
					val.uint32 = FWPM_NET_EVENT_KEYWORD_CLASSIFY_ALLOW |
						FWPM_NET_EVENT_KEYWORD_INBOUND_MCAST |
						FWPM_NET_EVENT_KEYWORD_INBOUND_BCAST;

					// 1903+
					if (_r_sys_validversion (10, 0, 18362))
						val.uint32 |= FWPM_NET_EVENT_KEYWORD_PORT_SCANNING_DROP;

					rc = FwpmEngineSetOption (hengine, FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, &val);

					if (rc != ERROR_SUCCESS)
						app.LogError (L"FwpmEngineSetOption", rc, L"FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS", 0);

					// enables the connection monitoring feature and starts logging creation and deletion events (and notifying any subscribers)
					if (app.ConfigGet (L"IsMonitorIPSecConnections", true).AsBool ())
					{
						val.type = FWP_UINT32;
						val.uint32 = 1;

						rc = FwpmEngineSetOption (hengine, FWPM_ENGINE_MONITOR_IPSEC_CONNECTIONS, &val);

						if (rc != ERROR_SUCCESS)
							app.LogError (L"FwpmEngineSetOption", rc, L"FWPM_ENGINE_MONITOR_IPSEC_CONNECTIONS", 0);
					}
				}

				config.is_neteventset = true;

				_wfp_logsubscribe (hengine);
			}
		}

		// packet queuing (win8+)
		if (is_win8 && app.ConfigGet (L"IsPacketQueuingEnabled", true).AsBool ())
		{
			// enables inbound or forward packet queuing independently. when enabled, the system is able to evenly distribute cpu load to multiple cpus for site-to-site ipsec tunnel scenarios.
			val.type = FWP_UINT32;
			val.uint32 = FWPM_ENGINE_OPTION_PACKET_QUEUE_INBOUND | FWPM_ENGINE_OPTION_PACKET_QUEUE_FORWARD;

			rc = FwpmEngineSetOption (hengine, FWPM_ENGINE_PACKET_QUEUING, &val);

			if (rc != ERROR_SUCCESS)
				app.LogError (L"FwpmEngineSetOption", rc, L"FWPM_ENGINE_PACKET_QUEUING", 0);
		}
	}

DoExit:

	_r_fastlock_releaseshared (&lock_transaction);

	return result;
}

void _wfp_uninitialize (HANDLE hengine, bool is_full)
{
	_r_fastlock_acquireshared (&lock_transaction);

	DWORD rc;

	// dropped packets logging (win7+)
	if (config.is_neteventset)
	{
		_wfp_logunsubscribe (hengine);

		//if (_r_sys_validversion (6, 2))
		//{
		//	// monitor ipsec connection (win8+)
		//	val.type = FWP_UINT32;
		//	val.uint32 = 0;

		//	FwpmEngineSetOption (hengine, FWPM_ENGINE_MONITOR_IPSEC_CONNECTIONS, &val);

		//	// packet queuing (win8+)
		//	val.type = FWP_UINT32;
		//	val.uint32 = FWPM_ENGINE_OPTION_PACKET_QUEUE_NONE;

		//	FwpmEngineSetOption (hengine, FWPM_ENGINE_PACKET_QUEUING, &val);
		//}

		//val.type = FWP_UINT32;
		//val.uint32 = 0;

		//rc = FwpmEngineSetOption (hengine, FWPM_ENGINE_COLLECT_NET_EVENTS, &val);

		//if (rc == ERROR_SUCCESS)
	}

	if (is_full)
	{
		_app_setsecurityinfoforprovider (hengine, &GUID_WfpProvider, false);
		_app_setsecurityinfoforsublayer (hengine, &GUID_WfpSublayer, false);

		const bool is_intransact = _wfp_transact_start (hengine, __LINE__);

		// destroy callouts (deprecated)
		{
			const GUID callouts[] = {
				GUID_WfpOutboundCallout4_DEPRECATED,
				GUID_WfpOutboundCallout6_DEPRECATED,
				GUID_WfpInboundCallout4_DEPRECATED,
				GUID_WfpInboundCallout6_DEPRECATED,
				GUID_WfpListenCallout4_DEPRECATED,
				GUID_WfpListenCallout6_DEPRECATED
			};

			for (auto callout_id : callouts)
				FwpmCalloutDeleteByKey (hengine, &callout_id);
		}

		// destroy sublayer
		rc = FwpmSubLayerDeleteByKey (hengine, &GUID_WfpSublayer);

		if (rc != ERROR_SUCCESS && rc != FWP_E_SUBLAYER_NOT_FOUND)
			app.LogError (L"FwpmSubLayerDeleteByKey", rc, nullptr, UID);

		// destroy provider
		rc = FwpmProviderDeleteByKey (hengine, &GUID_WfpProvider);

		if (rc != ERROR_SUCCESS && rc != FWP_E_PROVIDER_NOT_FOUND)
			app.LogError (L"FwpmProviderDeleteByKey", rc, nullptr, UID);

		if (is_intransact)
			_wfp_transact_commit (hengine, __LINE__);
	}

	_r_fastlock_releaseshared (&lock_transaction);
}

void _wfp_installfilters (HANDLE hengine)
{
	// set security information
	_app_setsecurityinfoforprovider (hengine, &GUID_WfpProvider, false);
	_app_setsecurityinfoforsublayer (hengine, &GUID_WfpSublayer, false);

	_wfp_clearfilter_ids ();

	_r_fastlock_acquireshared (&lock_transaction);

	// dump all filters into array
	GUIDS_VEC filter_all;
	const bool filters_count = _wfp_dumpfilters (hengine, &GUID_WfpProvider, &filter_all);

	// restore filters security
	if (filters_count)
	{
		for (auto filter_id : filter_all)
			_app_setsecurityinfoforfilter (hengine, &filter_id, false, __LINE__);
	}

	const bool is_intransact = _wfp_transact_start (hengine, __LINE__);

	// destroy all filters
	if (filters_count)
	{
		for (auto &p : filter_all)
			_wfp_deletefilter (hengine, &p);
	}

	// apply internal rules
	_wfp_create2filters (hengine, __LINE__, is_intransact);

	// apply apps rules
	{
		OBJECTS_VEC rules;

		for (auto &p : apps)
		{
			PR_OBJECT ptr_app_object = _r_obj_reference (p.second);

			if (!ptr_app_object)
				continue;

			PITEM_APP ptr_app = (PITEM_APP)ptr_app_object->pdata;

			if (ptr_app && ptr_app->is_enabled)
			{
				rules.push_back (ptr_app_object);
			}
			else
			{
				_r_obj_dereference (ptr_app_object);
			}
		}

		if (!rules.empty ())
		{
			_wfp_create3filters (hengine, rules, __LINE__, is_intransact);
			_app_freeobjects_vec (rules);
		}
	}

	// apply blocklist/system/user rules
	{
		OBJECTS_VEC rules;

		for (auto &p : rules_arr)
		{
			PR_OBJECT ptr_rule_object = _r_obj_reference (p);

			if (!ptr_rule_object)
				continue;

			const PITEM_RULE ptr_rule = (PITEM_RULE)ptr_rule_object->pdata;

			if (ptr_rule && ptr_rule->is_enabled)
			{
				rules.push_back (ptr_rule_object);
			}
			else
			{
				_r_obj_dereference (ptr_rule_object);
			}
		}

		if (!rules.empty ())
		{
			_wfp_create4filters (hengine, rules, __LINE__, is_intransact);
			_app_freeobjects_vec (rules);
		}
	}

	if (is_intransact)
		_wfp_transact_commit (hengine, __LINE__);

	// secure filters
	const bool is_secure = app.ConfigGet (L"IsSecureFilters", true).AsBool ();

	if (_wfp_dumpfilters (hengine, &GUID_WfpProvider, &filter_all))
	{
		for (auto filter_id : filter_all)
			_app_setsecurityinfoforfilter (hengine, &filter_id, is_secure, __LINE__);
	}

	_app_setsecurityinfoforprovider (hengine, &GUID_WfpProvider, is_secure);
	_app_setsecurityinfoforsublayer (hengine, &GUID_WfpSublayer, is_secure);

	_r_fastlock_releaseshared (&lock_transaction);
}

bool _wfp_transact_start (HANDLE hengine, UINT line)
{
	DWORD rc = FwpmTransactionBegin (hengine, 0);

	//if (rc == FWP_E_TXN_IN_PROGRESS)
	//	return false;

	if (rc != ERROR_SUCCESS)
	{
		app.LogError (L"FwpmTransactionBegin", rc, _r_fmt (L"#%d", line), UID);
		return false;
	}

	return true;
}

bool _wfp_transact_commit (HANDLE hengine, UINT line)
{
	DWORD rc = FwpmTransactionCommit (hengine);

	if (rc != ERROR_SUCCESS)
	{
		FwpmTransactionAbort (hengine);

		app.LogError (L"FwpmTransactionCommit", rc, _r_fmt (L"#%d", line), UID);
		return false;

	}

	return true;
}

bool _wfp_deletefilter (HANDLE hengine, const LPGUID pfilter_id)
{
	if (!hengine || !pfilter_id)
		return false;

	const DWORD rc = FwpmFilterDeleteByKey (hengine, pfilter_id);

	if (rc != ERROR_SUCCESS && rc != FWP_E_FILTER_NOT_FOUND)
	{
		app.LogError (L"FwpmFilterDeleteByKey", rc, _r_str_fromguid (*pfilter_id).GetString (), UID);
		return false;
	}

	return true;
}

DWORD _wfp_createfilter (HANDLE hengine, LPCWSTR name, FWPM_FILTER_CONDITION* lpcond, UINT32 count, UINT8 weight, const GUID* layer_id, const GUID* callout_id, FWP_ACTION_TYPE action, UINT32 flags, GUIDS_VEC* ptr_filters)
{
	FWPM_FILTER filter = {0};

	WCHAR fltr_name[128] = {0};
	_r_str_copy (fltr_name, _countof (fltr_name), APP_NAME);

	WCHAR fltr_desc[128] = {0};
	_r_str_copy (fltr_desc, _countof (fltr_desc), _r_str_isempty (name) ? SZ_EMPTY : name);

	filter.displayData.name = fltr_name;
	filter.displayData.description = fltr_desc;

	// set filter flags
	if ((flags & FWPM_FILTER_FLAG_BOOTTIME) == 0)
	{
		if (!config.is_filterstemporary)
			filter.flags = FWPM_FILTER_FLAG_PERSISTENT;

		// filter is indexed to help enable faster lookup during classification (win8+)
		if (_r_sys_validversion (6, 2))
			filter.flags |= FWPM_FILTER_FLAG_INDEXED;
	}

	if (flags)
		filter.flags |= flags;

	filter.providerKey = (LPGUID)&GUID_WfpProvider;
	filter.subLayerKey = GUID_WfpSublayer;
	CoCreateGuid (&filter.filterKey); // set filter guid

	if (count)
	{
		filter.numFilterConditions = count;
		filter.filterCondition = lpcond;
	}

	if (layer_id)
		RtlCopyMemory (&filter.layerKey, layer_id, sizeof (GUID));

	if (callout_id)
		RtlCopyMemory (&filter.action.calloutKey, callout_id, sizeof (GUID));

	filter.weight.type = FWP_UINT8;
	filter.weight.uint8 = weight;

	filter.action.type = action;

	UINT64 filter_id;
	const DWORD rc = FwpmFilterAdd (hengine, &filter, nullptr, &filter_id);

	if (rc == ERROR_SUCCESS)
	{
		if (ptr_filters)
			ptr_filters->push_back (filter.filterKey);
	}
	else
	{
		app.LogError (L"FwpmFilterAdd", rc, fltr_desc, UID);
	}

	return rc;
}

void _wfp_clearfilter_ids ()
{
	// clear common filters
	filter_ids.clear ();

	// clear apps filters
	for (auto p : apps)
	{
		PR_OBJECT ptr_app_object = _r_obj_reference (p.second);

		if (!ptr_app_object)
			continue;

		PITEM_APP ptr_app = (PITEM_APP)ptr_app_object->pdata;

		if (ptr_app)
		{
			ptr_app->is_haveerrors = false;
			ptr_app->guids.clear ();
		}

		_r_obj_dereference (ptr_app_object);
	}

	// clear rules filters
	for (auto p : rules_arr)
	{
		PR_OBJECT ptr_rule_object = _r_obj_reference (p);

		if (!ptr_rule_object)
			continue;

		PITEM_RULE ptr_rule = (PITEM_RULE)ptr_rule_object->pdata;

		if (ptr_rule)
		{
			ptr_rule->is_haveerrors = false;
			ptr_rule->guids.clear ();
		}

		_r_obj_dereference (ptr_rule_object);
	}
}

void _wfp_destroyfilters (HANDLE hengine)
{
	_wfp_clearfilter_ids ();

	// destroy all filters
	GUIDS_VEC filter_all;

	_r_fastlock_acquireshared (&lock_transaction);

	if (_wfp_dumpfilters (hengine, &GUID_WfpProvider, &filter_all))
		_wfp_destroyfilters_array (hengine, filter_all, __LINE__);

	_r_fastlock_releaseshared (&lock_transaction);
}

bool _wfp_destroyfilters_array (HANDLE hengine, GUIDS_VEC& ptr_filters, UINT line)
{
	if (ptr_filters.empty ())
		return false;

	const bool is_enabled = _app_initinterfacestate (app.GetHWND (), false);

	_r_fastlock_acquireshared (&lock_transaction);

	for (auto filter_id : ptr_filters)
		_app_setsecurityinfoforfilter (hengine, &filter_id, false, line);

	const bool is_intransact = _wfp_transact_start (hengine, line);

	for (auto filter_id : ptr_filters)
		_wfp_deletefilter (hengine, &filter_id);

	ptr_filters.clear ();

	if (is_intransact)
		_wfp_transact_commit (hengine, line);

	_r_fastlock_releaseshared (&lock_transaction);

	_app_restoreinterfacestate (app.GetHWND (), is_enabled);

	return true;
}

bool _wfp_createrulefilter (HANDLE hengine, LPCWSTR name, size_t app_hash, LPCWSTR rule_remote, LPCWSTR rule_local, UINT8 protocol, ADDRESS_FAMILY af, FWP_DIRECTION dir, UINT8 weight, FWP_ACTION_TYPE action, UINT32 flag, GUIDS_VEC* pmfarr)
{
	UINT32 count = 0;
	FWPM_FILTER_CONDITION fwfc[8] = {0};

	FWP_BYTE_BLOB* bPath = nullptr;
	FWP_BYTE_BLOB* bSid = nullptr;

	FWP_V4_ADDR_AND_MASK addr4 = {0};
	FWP_V6_ADDR_AND_MASK addr6 = {0};

	FWP_RANGE range;
	RtlSecureZeroMemory (&range, sizeof (range));

	ITEM_ADDRESS addr;
	RtlSecureZeroMemory (&addr, sizeof (addr));

	addr.paddr4 = &addr4;
	addr.paddr6 = &addr6;
	addr.prange = &range;

	bool is_remoteaddr_set = false;
	bool is_remoteport_set = false;

	// set path condition
	if (app_hash)
	{
		PR_OBJECT ptr_app_object = _app_getappitem (app_hash);

		if (!ptr_app_object)
		{
			app.LogError (TEXT (__FUNCTION__), 0, _r_fmt (L"App \"%" PR_SIZE_T L"\" not found!", app_hash), 0);
			return false;
		}

		PITEM_APP ptr_app = (PITEM_APP)ptr_app_object->pdata;

		if (!ptr_app)
		{
			app.LogError (TEXT (__FUNCTION__), 0, _r_fmt (L"App \"%" PR_SIZE_T L"\" not found!", app_hash), 0);
			_r_obj_dereference (ptr_app_object);

			return false;
		}

		if (ptr_app->type == DataAppUWP) // windows store app (win8+)
		{
			PVOID pdata = nullptr;

			if (_app_item_get (ptr_app->type, app_hash, nullptr, nullptr, nullptr, &pdata))
			{
				fwfc[count].fieldKey = FWPM_CONDITION_ALE_PACKAGE_ID;
				fwfc[count].matchType = FWP_MATCH_EQUAL;
				fwfc[count].conditionValue.type = FWP_SID;
				fwfc[count].conditionValue.sid = (SID*)pdata;

				count += 1;
			}
			else
			{
				app.LogError (TEXT (__FUNCTION__), 0, ptr_app->display_name, 0);
				_r_obj_dereference (ptr_app_object);

				return false;
			}
		}
		else if (ptr_app->type == DataAppService) // windows service
		{
			PVOID pdata = nullptr;

			if (_app_item_get (ptr_app->type, app_hash, nullptr, nullptr, nullptr, &pdata) && ByteBlobAlloc (pdata, GetSecurityDescriptorLength (pdata), &bSid))
			{
				fwfc[count].fieldKey = FWPM_CONDITION_ALE_USER_ID;
				fwfc[count].matchType = FWP_MATCH_EQUAL;
				fwfc[count].conditionValue.type = FWP_SECURITY_DESCRIPTOR_TYPE;
				fwfc[count].conditionValue.sd = bSid;

				count += 1;
			}
			else
			{
				ByteBlobFree (&bPath);
				ByteBlobFree (&bSid);

				app.LogError (TEXT (__FUNCTION__), 0, ptr_app->display_name, 0);
				_r_obj_dereference (ptr_app_object);

				return false;
			}
		}
		else
		{
			LPCWSTR path = ptr_app->original_path;
			const DWORD rc = _FwpmGetAppIdFromFileName1 (path, &bPath, ptr_app->type);

			if (rc == ERROR_SUCCESS)
			{
				fwfc[count].fieldKey = FWPM_CONDITION_ALE_APP_ID;
				fwfc[count].matchType = FWP_MATCH_EQUAL;
				fwfc[count].conditionValue.type = FWP_BYTE_BLOB_TYPE;
				fwfc[count].conditionValue.byteBlob = bPath;

				count += 1;
			}
			else
			{
				ByteBlobFree (&bSid);
				ByteBlobFree (&bPath);

				// do not log file not found to error log
				if (rc != ERROR_FILE_NOT_FOUND && rc != ERROR_PATH_NOT_FOUND)
					app.LogError (L"FwpmGetAppIdFromFileName", rc, path, 0);

				_r_obj_dereference (ptr_app_object);

				return false;
			}
		}

		_r_obj_dereference (ptr_app_object);
	}

	// set ip/port condition
	{
		LPCWSTR rules[] = {
			rule_remote,
			rule_local
		};

		for (size_t i = 0; i < _countof (rules); i++)
		{
			if (!_r_str_isempty (rules[i]))
			{
				if (!_app_parserulestring (rules[i], &addr))
				{
					ByteBlobFree (&bSid);
					ByteBlobFree (&bPath);

					return false;
				}
				else
				{
					if (i == 0)
					{
						if (addr.type == DataTypeIp || addr.type == DataTypeHost)
							is_remoteaddr_set = true;

						else if (addr.type == DataTypePort)
							is_remoteport_set = true;
					}

					if (addr.is_range && (addr.type == DataTypeIp || addr.type == DataTypePort))
					{
						if (addr.type == DataTypeIp)
						{
							if (addr.format == NET_ADDRESS_IPV4)
							{
								af = AF_INET;
							}
							else if (addr.format == NET_ADDRESS_IPV6)
							{
								af = AF_INET6;
							}
							else
							{
								ByteBlobFree (&bSid);
								ByteBlobFree (&bPath);

								return false;
							}
						}

						fwfc[count].fieldKey = (addr.type == DataTypePort) ? ((i == 0) ? FWPM_CONDITION_IP_REMOTE_PORT : FWPM_CONDITION_IP_LOCAL_PORT) : ((i == 0) ? FWPM_CONDITION_IP_REMOTE_ADDRESS : FWPM_CONDITION_IP_LOCAL_ADDRESS);
						fwfc[count].matchType = FWP_MATCH_RANGE;
						fwfc[count].conditionValue.type = FWP_RANGE_TYPE;
						fwfc[count].conditionValue.rangeValue = &range;

						count += 1;
					}
					else if (addr.type == DataTypePort)
					{
						fwfc[count].fieldKey = ((i == 0) ? FWPM_CONDITION_IP_REMOTE_PORT : FWPM_CONDITION_IP_LOCAL_PORT);
						fwfc[count].matchType = FWP_MATCH_EQUAL;
						fwfc[count].conditionValue.type = FWP_UINT16;
						fwfc[count].conditionValue.uint16 = addr.port;

						count += 1;
					}
					else if (addr.type == DataTypeHost || addr.type == DataTypeIp)
					{
						fwfc[count].fieldKey = ((i == 0) ? FWPM_CONDITION_IP_REMOTE_ADDRESS : FWPM_CONDITION_IP_LOCAL_ADDRESS);
						fwfc[count].matchType = FWP_MATCH_EQUAL;

						if (addr.format == NET_ADDRESS_IPV4)
						{
							af = AF_INET;

							fwfc[count].conditionValue.type = FWP_V4_ADDR_MASK;
							fwfc[count].conditionValue.v4AddrMask = &addr4;

							count += 1;
						}
						else if (addr.format == NET_ADDRESS_IPV6)
						{
							af = AF_INET6;

							fwfc[count].conditionValue.type = FWP_V6_ADDR_MASK;
							fwfc[count].conditionValue.v6AddrMask = &addr6;

							count += 1;
						}
						else if (addr.format == NET_ADDRESS_DNS_NAME)
						{
							ByteBlobFree (&bSid);
							ByteBlobFree (&bPath);

							rstringvec rvc;
							_r_str_split (addr.host, INVALID_SIZE_T, DIVIDER_RULE[0], rvc);

							if (rvc.empty ())
							{
								return false;
							}
							else
							{
								for (auto &p : rvc)
								{
									if (!_wfp_createrulefilter (hengine, name, app_hash, p, nullptr, protocol, af, dir, weight, action, flag, pmfarr))
										return false;
								}
							}

							return true;
						}
						else
						{
							ByteBlobFree (&bSid);
							ByteBlobFree (&bPath);

							return false;
						}

						// set port if available
						if (addr.port)
						{
							fwfc[count].fieldKey = ((i == 0) ? FWPM_CONDITION_IP_REMOTE_PORT : FWPM_CONDITION_IP_LOCAL_PORT);
							fwfc[count].matchType = FWP_MATCH_EQUAL;
							fwfc[count].conditionValue.type = FWP_UINT16;
							fwfc[count].conditionValue.uint16 = addr.port;

							count += 1;
						}
					}
					else
					{
						ByteBlobFree (&bSid);
						ByteBlobFree (&bPath);

						return false;
					}
				}
			}
		}
	}

	// set protocol condition
	if (protocol)
	{
		fwfc[count].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
		fwfc[count].matchType = FWP_MATCH_EQUAL;
		fwfc[count].conditionValue.type = FWP_UINT8;
		fwfc[count].conditionValue.uint8 = protocol;

		count += 1;
	}

	// create outbound layer filter
	if (dir == FWP_DIRECTION_OUTBOUND || dir == FWP_DIRECTION_MAX)
	{
		if (af == AF_INET || af == AF_UNSPEC)
		{
			_wfp_createfilter (hengine, name, fwfc, count, weight, &FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, action, flag, pmfarr);

			// win7+
			_wfp_createfilter (hengine, name, fwfc, count, weight, &FWPM_LAYER_ALE_CONNECT_REDIRECT_V4, nullptr, action, flag, pmfarr);
		}

		if (af == AF_INET6 || af == AF_UNSPEC)
		{
			_wfp_createfilter (hengine, name, fwfc, count, weight, &FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, action, flag, pmfarr);

			// win7+
			_wfp_createfilter (hengine, name, fwfc, count, weight, &FWPM_LAYER_ALE_CONNECT_REDIRECT_V6, nullptr, action, flag, pmfarr);
		}
	}

	// create inbound layer filter
	if (dir == FWP_DIRECTION_INBOUND || dir == FWP_DIRECTION_MAX)
	{
		if (af == AF_INET || af == AF_UNSPEC)
		{
			_wfp_createfilter (hengine, name, fwfc, count, weight, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, action, flag, pmfarr);

			if (action == FWP_ACTION_BLOCK && (!is_remoteaddr_set && !is_remoteport_set))
				_wfp_createfilter (hengine, name, fwfc, count, weight, &FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4, nullptr, FWP_ACTION_PERMIT, flag, pmfarr);
		}

		if (af == AF_INET6 || af == AF_UNSPEC)
		{
			_wfp_createfilter (hengine, name, fwfc, count, weight, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, action, flag, pmfarr);

			if (action == FWP_ACTION_BLOCK && (!is_remoteaddr_set && !is_remoteport_set))
				_wfp_createfilter (hengine, name, fwfc, count, weight, &FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V6, nullptr, FWP_ACTION_PERMIT, flag, pmfarr);
		}
	}

	ByteBlobFree (&bSid);
	ByteBlobFree (&bPath);

	return true;
}

bool _wfp_create4filters (HANDLE hengine, const OBJECTS_VEC& ptr_rules, UINT line, bool is_intransact)
{
	if (ptr_rules.empty ())
		return false;

	const bool is_enabled = _app_initinterfacestate (app.GetHWND (), false);

	if (!is_intransact && _wfp_isfiltersapplying ())
		is_intransact = true;

	GUIDS_VEC ids;

	if (!is_intransact)
	{
		for (auto &p : ptr_rules)
		{
			PR_OBJECT ptr_rule_object = _r_obj_reference (p);

			if (!ptr_rule_object)
				continue;

			const PITEM_RULE ptr_rule = (PITEM_RULE)ptr_rule_object->pdata;

			if (ptr_rule)
			{
				if (!ptr_rule->guids.empty ())
				{
					ids.insert (ids.end (), ptr_rule->guids.begin (), ptr_rule->guids.end ());
					ptr_rule->guids.clear ();
				}
			}

			_r_obj_dereference (ptr_rule_object);
		}

		for (auto filter_id : ids)
			_app_setsecurityinfoforfilter (hengine, &filter_id, false, line);

		_r_fastlock_acquireshared (&lock_transaction);
		is_intransact = !_wfp_transact_start (hengine, line);
	}

	for (auto &p : ids)
		_wfp_deletefilter (hengine, &p);

	for (auto &p : ptr_rules)
	{
		PR_OBJECT ptr_rule_object = _r_obj_reference (p);

		if (!ptr_rule_object)
			continue;

		PITEM_RULE ptr_rule = (PITEM_RULE)ptr_rule_object->pdata;

		if (ptr_rule)
		{
			GUIDS_VEC guids;
			bool is_haveerrors = false;

			if (ptr_rule->is_enabled)
			{
				rstringvec rule_remote_arr;
				_r_str_split (ptr_rule->prule_remote, INVALID_SIZE_T, DIVIDER_RULE[0], rule_remote_arr);

				rstringvec rule_local_arr;
				_r_str_split (ptr_rule->prule_local, INVALID_SIZE_T, DIVIDER_RULE[0], rule_local_arr);

				const size_t rules_remote_length = rule_remote_arr.size ();
				const size_t rules_local_length = rule_local_arr.size ();

				const size_t count = (std::max) (size_t (1), (std::max) (rules_remote_length, rules_local_length));

				for (size_t j = 0; j < count; j++)
				{
					rstring rule_remote;
					rstring rule_local;

					// sync remote rules and local rules
					if (!rule_remote_arr.empty () && rules_remote_length > j)
					{
						rule_remote = std::move (rule_remote_arr.at (j));
						_r_str_trim (rule_remote, DIVIDER_TRIM);
					}

					// sync local rules and remote rules
					if (!rule_local_arr.empty () && rules_local_length > j)
					{
						rule_local = std::move (rule_local_arr.at (j));
						_r_str_trim (rule_local, DIVIDER_TRIM);
					}

					// apply rules for services hosts
					if (ptr_rule->is_forservices)
					{
						if (!_wfp_createrulefilter (hengine, ptr_rule->pname, config.ntoskrnl_hash, rule_remote, rule_local, ptr_rule->protocol, ptr_rule->af, ptr_rule->direction, ptr_rule->weight, ptr_rule->is_block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT, 0, &guids))
							is_haveerrors = true;

						if (!_wfp_createrulefilter (hengine, ptr_rule->pname, config.svchost_hash, rule_remote, rule_local, ptr_rule->protocol, ptr_rule->af, ptr_rule->direction, ptr_rule->weight, ptr_rule->is_block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT, 0, &guids))
							is_haveerrors = true;
					}

					if (!ptr_rule->apps.empty ())
					{
						for (auto const &papps : ptr_rule->apps)
						{
							if (ptr_rule->is_forservices && (papps.first == config.ntoskrnl_hash || papps.first == config.svchost_hash))
								continue;

							if (!_wfp_createrulefilter (hengine, ptr_rule->pname, papps.first, rule_remote, rule_local, ptr_rule->protocol, ptr_rule->af, ptr_rule->direction, ptr_rule->weight, ptr_rule->is_block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT, 0, &guids))
								is_haveerrors = true;
						}
					}
					else
					{
						if (!_wfp_createrulefilter (hengine, ptr_rule->pname, 0, rule_remote, rule_local, ptr_rule->protocol, ptr_rule->af, ptr_rule->direction, ptr_rule->weight, ptr_rule->is_block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT, 0, &guids))
							is_haveerrors = true;
					}
				}
			}

			ptr_rule->is_haveerrors = is_haveerrors;

			ptr_rule->guids.clear ();
			ptr_rule->guids = std::move (guids);
		}

		_r_obj_dereference (ptr_rule_object);
	}

	if (!is_intransact)
	{
		_wfp_transact_commit (hengine, line);

		const bool is_secure = app.ConfigGet (L"IsSecureFilters", true).AsBool ();

		for (auto &p : ptr_rules)
		{
			PR_OBJECT ptr_rule_object = _r_obj_reference (p);

			if (!ptr_rule_object)
				continue;

			const PITEM_RULE ptr_rule = (PITEM_RULE)ptr_rule_object->pdata;

			if (ptr_rule && ptr_rule->is_enabled)
			{
				for (auto filter_id : ptr_rule->guids)
					_app_setsecurityinfoforfilter (hengine, &filter_id, is_secure, line);
			}

			_r_obj_dereference (ptr_rule_object);
		}

		_r_fastlock_releaseshared (&lock_transaction);
	}

	_app_restoreinterfacestate (app.GetHWND (), is_enabled);

	return true;
}

bool _wfp_create3filters (HANDLE hengine, const OBJECTS_VEC& ptr_apps, UINT line, bool is_intransact)
{
	if (ptr_apps.empty ())
		return false;

	const bool is_enabled = _app_initinterfacestate (app.GetHWND (), false);
	const FWP_ACTION_TYPE action = FWP_ACTION_PERMIT;

	if (!is_intransact && _wfp_isfiltersapplying ())
		is_intransact = true;

	GUIDS_VEC ids;

	if (!is_intransact)
	{
		for (auto &p : ptr_apps)
		{
			PR_OBJECT ptr_app_object = _r_obj_reference (p);

			if (!ptr_app_object)
				continue;

			PITEM_APP ptr_app = (PITEM_APP)ptr_app_object->pdata;

			if (ptr_app)
			{
				if (!ptr_app->guids.empty ())
				{
					ids.insert (ids.end (), ptr_app->guids.begin (), ptr_app->guids.end ());
					ptr_app->guids.clear ();
				}
			}

			_r_obj_dereference (ptr_app_object);
		}

		for (auto filter_id : ids)
			_app_setsecurityinfoforfilter (hengine, &filter_id, false, line);

		_r_fastlock_acquireshared (&lock_transaction);
		is_intransact = !_wfp_transact_start (hengine, line);
	}

	for (auto filter_id : ids)
		_wfp_deletefilter (hengine, &filter_id);

	for (auto &p : ptr_apps)
	{
		PR_OBJECT ptr_app_object = _r_obj_reference (p);

		if (!ptr_app_object)
			continue;

		PITEM_APP ptr_app = (PITEM_APP)ptr_app_object->pdata;

		if (ptr_app)
		{
			GUIDS_VEC guids;

			if (ptr_app->is_enabled)
			{
				if (!_wfp_createrulefilter (hengine, ptr_app->display_name, _r_str_hash (ptr_app->original_path), nullptr, nullptr, 0, AF_UNSPEC, FWP_DIRECTION_MAX, FILTER_WEIGHT_APPLICATION, action, 0, &guids))
					ptr_app->is_haveerrors = true;
			}

			ptr_app->guids.clear ();

			if (!guids.empty ())
				ptr_app->guids = std::move (guids);
		}

		_r_obj_dereference (ptr_app_object);
	}

	if (!is_intransact)
	{
		_wfp_transact_commit (hengine, line);

		const bool is_secure = app.ConfigGet (L"IsSecureFilters", true).AsBool ();

		for (auto &p : ptr_apps)
		{
			PR_OBJECT ptr_app_object = _r_obj_reference (p);

			if (!ptr_app_object)
				continue;

			PITEM_APP ptr_app = (PITEM_APP)ptr_app_object->pdata;

			if (ptr_app)
			{
				for (auto filter_id : ptr_app->guids)
					_app_setsecurityinfoforfilter (hengine, &filter_id, is_secure, line);
			}

			_r_obj_dereference (ptr_app_object);
		}

		_r_fastlock_releaseshared (&lock_transaction);
	}

	_app_restoreinterfacestate (app.GetHWND (), is_enabled);

	return true;
}

bool _wfp_create2filters (HANDLE hengine, UINT line, bool is_intransact)
{
	const bool is_enabled = _app_initinterfacestate (app.GetHWND (), false);

	if (!is_intransact && _wfp_isfiltersapplying ())
		is_intransact = true;

	if (!is_intransact)
	{
		for (auto filter_id : filter_ids)
			_app_setsecurityinfoforfilter (hengine, &filter_id, false, line);

		_r_fastlock_acquireshared (&lock_transaction);
		is_intransact = !_wfp_transact_start (hengine, line);
	}

	if (!filter_ids.empty ())
	{
		for (auto &p : filter_ids)
			_wfp_deletefilter (hengine, &p);

		filter_ids.clear ();
	}

	FWPM_FILTER_CONDITION fwfc[3] = {0};

	// add loopback connections permission
	if (app.ConfigGet (L"AllowLoopbackConnections", true).AsBool ())
	{
		// match all loopback (localhost) data
		fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
		fwfc[0].matchType = FWP_MATCH_FLAGS_ALL_SET;
		fwfc[0].conditionValue.type = FWP_UINT32;
		fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

		// tests if the network traffic is (non-)app container loopback traffic (win8+)
		if (_r_sys_validversion (6, 2))
		{
			fwfc[0].matchType = FWP_MATCH_FLAGS_ANY_SET;
			fwfc[0].conditionValue.uint32 |= FWP_CONDITION_FLAG_IS_APPCONTAINER_LOOPBACK;
		}

		_wfp_createfilter (hengine, nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);
		_wfp_createfilter (hengine, nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

		// win7+
		_wfp_createfilter (hengine, nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_CONNECT_REDIRECT_V4, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);
		_wfp_createfilter (hengine, nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_CONNECT_REDIRECT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

		_wfp_createfilter (hengine, nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);
		_wfp_createfilter (hengine, nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

		_wfp_createfilter (hengine, nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);
		_wfp_createfilter (hengine, nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

		// ipv4/ipv6 loopback
		LPCWSTR ip_list[] = {
			L"0.0.0.0/8",
			L"10.0.0.0/8",
			L"100.64.0.0/10",
			L"127.0.0.0/8",
			L"169.254.0.0/16",
			L"172.16.0.0/12",
			L"192.0.0.0/24",
			L"192.0.2.0/24",
			L"192.88.99.0/24",
			L"192.168.0.0/16",
			L"198.18.0.0/15",
			L"198.51.100.0/24",
			L"203.0.113.0/24",
			L"224.0.0.0/4",
			L"240.0.0.0/4",
			L"255.255.255.255/32",
			L"::/0",
			L"::/128",
			L"::1/128",
			L"::ffff:0:0/96",
			L"::ffff:0:0:0/96",
			L"64:ff9b::/96",
			L"100::/64",
			L"2001::/32",
			L"2001:20::/28",
			L"2001:db8::/32",
			L"2002::/16",
			L"fc00::/7",
			L"fe80::/10",
			L"ff00::/8"
		};

		for (size_t i = 0; i < _countof (ip_list); i++)
		{
			FWP_V4_ADDR_AND_MASK addr4 = {0};
			FWP_V6_ADDR_AND_MASK addr6 = {0};

			ITEM_ADDRESS addr;
			RtlSecureZeroMemory (&addr, sizeof (addr));

			addr.paddr4 = &addr4;
			addr.paddr6 = &addr6;

			if (_app_parserulestring (ip_list[i], &addr))
			{
				fwfc[1].matchType = FWP_MATCH_EQUAL;

				if (addr.format == NET_ADDRESS_IPV4)
				{
					fwfc[1].conditionValue.type = FWP_V4_ADDR_MASK;
					fwfc[1].conditionValue.v4AddrMask = &addr4;

					fwfc[1].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
					_wfp_createfilter (hengine, nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

					// win7+
					_wfp_createfilter (hengine, nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_CONNECT_REDIRECT_V4, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

					fwfc[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
					_wfp_createfilter (hengine, nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);
					_wfp_createfilter (hengine, nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);
				}
				else if (addr.format == NET_ADDRESS_IPV6)
				{
					fwfc[1].conditionValue.type = FWP_V6_ADDR_MASK;
					fwfc[1].conditionValue.v6AddrMask = &addr6;

					fwfc[1].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
					_wfp_createfilter (hengine, nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

					// win7+
					_wfp_createfilter (hengine, nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_CONNECT_REDIRECT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

					fwfc[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
					_wfp_createfilter (hengine, nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);
					_wfp_createfilter (hengine, nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);
				}
			}
		}
	}

	// firewall service rules
	// https://msdn.microsoft.com/en-us/library/gg462153.aspx
	if (app.ConfigGet (L"AllowIPv6", true).AsBool ())
	{
		// allows 6to4 tunneling, which enables ipv6 to run over an ipv4 network
		fwfc[0].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
		fwfc[0].matchType = FWP_MATCH_EQUAL;
		fwfc[0].conditionValue.type = FWP_UINT8;
		fwfc[0].conditionValue.uint8 = IPPROTO_IPV6; // ipv6 header

		_wfp_createfilter (hengine, L"Allow6to4", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

		// allows icmpv6 router solicitation messages, which are required for the ipv6 stack to work properly
		fwfc[0].fieldKey = FWPM_CONDITION_ICMP_TYPE;
		fwfc[0].matchType = FWP_MATCH_EQUAL;
		fwfc[0].conditionValue.type = FWP_UINT16;
		fwfc[0].conditionValue.uint16 = 0x85;

		_wfp_createfilter (hengine, L"AllowIcmpV6Type133", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

		// allows icmpv6 router advertise messages, which are required for the ipv6 stack to work properly
		fwfc[0].conditionValue.uint16 = 0x86;
		_wfp_createfilter (hengine, L"AllowIcmpV6Type134", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

		// allows icmpv6 neighbor solicitation messages, which are required for the ipv6 stack to work properly
		fwfc[0].conditionValue.uint16 = 0x87;
		_wfp_createfilter (hengine, L"AllowIcmpV6Type135", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);

		// allows icmpv6 neighbor advertise messages, which are required for the ipv6 stack to work properly
		fwfc[0].conditionValue.uint16 = 0x88;
		_wfp_createfilter (hengine, L"AllowIcmpV6Type136", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FWP_ACTION_PERMIT, 0, &filter_ids);
	}

	// prevent port scanning using stealth discards and silent drops
	// https://docs.microsoft.com/ru-ru/windows/desktop/FWP/preventing-port-scanning
	if (app.ConfigGet (L"UseStealthMode", true).AsBool ())
	{
		// blocks udp port scanners
		fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
		fwfc[0].matchType = FWP_MATCH_FLAGS_NONE_SET;
		fwfc[0].conditionValue.type = FWP_UINT32;
		fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

		// tests if the network traffic is (non-)app container loopback traffic (win8+)
		if (_r_sys_validversion (6, 2))
			fwfc[0].conditionValue.uint32 |= FWP_CONDITION_FLAG_IS_APPCONTAINER_LOOPBACK;

		fwfc[1].fieldKey = FWPM_CONDITION_ICMP_TYPE;
		fwfc[1].matchType = FWP_MATCH_EQUAL;
		fwfc[1].conditionValue.type = FWP_UINT16;
		fwfc[1].conditionValue.uint16 = 0x03; // destination unreachable

		_wfp_createfilter (hengine, L"BlockIcmpErrorV4", fwfc, 2, FILTER_WEIGHT_HIGHEST, &FWPM_LAYER_OUTBOUND_ICMP_ERROR_V4, nullptr, FWP_ACTION_BLOCK, 0, &filter_ids);
		_wfp_createfilter (hengine, L"BlockIcmpErrorV6", fwfc, 2, FILTER_WEIGHT_HIGHEST, &FWPM_LAYER_OUTBOUND_ICMP_ERROR_V6, nullptr, FWP_ACTION_BLOCK, 0, &filter_ids);

		// blocks tcp port scanners (exclude loopback)
		fwfc[0].conditionValue.uint32 |= FWP_CONDITION_FLAG_IS_IPSEC_SECURED;

		_wfp_createfilter (hengine, L"BlockTcpRstOnCloseV4", fwfc, 1, FILTER_WEIGHT_HIGHEST, &FWPM_LAYER_INBOUND_TRANSPORT_V4_DISCARD, &FWPM_CALLOUT_WFP_TRANSPORT_LAYER_V4_SILENT_DROP, FWP_ACTION_CALLOUT_TERMINATING, 0, &filter_ids);
		_wfp_createfilter (hengine, L"BlockTcpRstOnCloseV6", fwfc, 1, FILTER_WEIGHT_HIGHEST, &FWPM_LAYER_INBOUND_TRANSPORT_V6_DISCARD, &FWPM_CALLOUT_WFP_TRANSPORT_LAYER_V6_SILENT_DROP, FWP_ACTION_CALLOUT_TERMINATING, 0, &filter_ids);
	}

	// configure outbound layer
	{
		const FWP_ACTION_TYPE action = app.ConfigGet (L"BlockOutboundConnections", true).AsBool () ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT;

		_wfp_createfilter (hengine, L"BlockConnectionsV4", nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, action, 0, &filter_ids);
		_wfp_createfilter (hengine, L"BlockConnectionsV6", nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, action, 0, &filter_ids);

		// win7+
		_wfp_createfilter (hengine, L"BlockConnectionsRedirectionV4", nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_ALE_CONNECT_REDIRECT_V4, nullptr, action, 0, &filter_ids);
		_wfp_createfilter (hengine, L"BlockConnectionsRedirectionV6", nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_ALE_CONNECT_REDIRECT_V6, nullptr, action, 0, &filter_ids);
	}

	// configure inbound layer
	{
		const FWP_ACTION_TYPE action = (app.ConfigGet (L"UseStealthMode", true).AsBool () || app.ConfigGet (L"BlockInboundConnections", true).AsBool ()) ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT;

		_wfp_createfilter (hengine, L"BlockRecvAcceptConnectionsV4", nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, action, 0, &filter_ids);
		_wfp_createfilter (hengine, L"BlockRecvAcceptConnectionsV6", nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, action, 0, &filter_ids);

		//_wfp_createfilter (L"BlockResourceAssignmentV4", nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4, nullptr, action, 0, &filter_ids);
		//_wfp_createfilter (L"BlockResourceAssignmentV6", nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V6, nullptr, action, 0, &filter_ids);
	}

	// install boot-time filters (enforced at boot-time, even before "base filtering engine" service starts)
	if (app.ConfigGet (L"InstallBoottimeFilters", true).AsBool () && !config.is_filterstemporary)
	{
		fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
		fwfc[0].matchType = FWP_MATCH_FLAGS_ALL_SET;
		fwfc[0].conditionValue.type = FWP_UINT32;
		fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

		// tests if the network traffic is (non-)app container loopback traffic (win8+)
		if (_r_sys_validversion (6, 2))
		{
			fwfc[0].matchType = FWP_MATCH_FLAGS_ANY_SET;
			fwfc[0].conditionValue.uint32 |= FWP_CONDITION_FLAG_IS_APPCONTAINER_LOOPBACK;
		}

		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_IPFORWARD_V4, nullptr, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);
		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_IPFORWARD_V6, nullptr, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);

		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);
		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);

		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_INBOUND_ICMP_ERROR_V4, nullptr, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);
		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_INBOUND_ICMP_ERROR_V6, nullptr, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);

		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_OUTBOUND_ICMP_ERROR_V4, nullptr, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);
		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, &FWPM_LAYER_OUTBOUND_ICMP_ERROR_V6, nullptr, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);

		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);
		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);

		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_INBOUND_ICMP_ERROR_V4, nullptr, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);
		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_INBOUND_ICMP_ERROR_V6, nullptr, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);

		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_OUTBOUND_ICMP_ERROR_V4, nullptr, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);
		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, nullptr, 0, FILTER_WEIGHT_LOWEST, &FWPM_LAYER_OUTBOUND_ICMP_ERROR_V6, nullptr, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);

		// win7+ boot-time features
		fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
		fwfc[0].matchType = FWP_MATCH_FLAGS_NONE_SET;
		fwfc[0].conditionValue.type = FWP_UINT32;
		fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_OUTBOUND_PASS_THRU | FWP_CONDITION_FLAG_IS_INBOUND_PASS_THRU;

		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, fwfc, 1, FILTER_WEIGHT_APPLICATION, &FWPM_LAYER_IPFORWARD_V4, nullptr, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);
		_wfp_createfilter (hengine, BOOTTIME_FILTER_NAME, fwfc, 1, FILTER_WEIGHT_APPLICATION, &FWPM_LAYER_IPFORWARD_V6, nullptr, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME, &filter_ids);
	}

	if (!is_intransact)
	{
		_wfp_transact_commit (hengine, line);

		const bool is_secure = app.ConfigGet (L"IsSecureFilters", true).AsBool ();

		for (auto filter_id : filter_ids)
			_app_setsecurityinfoforfilter (hengine, &filter_id, is_secure, line);

		_r_fastlock_releaseshared (&lock_transaction);
	}

	_app_restoreinterfacestate (app.GetHWND (), is_enabled);

	return true;
}

size_t _wfp_dumpfilters (HANDLE hengine, const GUID* pprovider_id, GUIDS_VEC* ptr_filters)
{
	if (!pprovider_id || !ptr_filters)
		return 0;

	ptr_filters->clear ();

	UINT32 count = 0;
	HANDLE henum = nullptr;

	DWORD rc = FwpmFilterCreateEnumHandle (hengine, nullptr, &henum);

	if (rc != ERROR_SUCCESS)
	{
		app.LogError (L"FwpmFilterCreateEnumHandle", rc, nullptr, 0);
		return 0;
	}
	else
	{
		FWPM_FILTER** matchingFwpFilter = nullptr;

		rc = FwpmFilterEnum (hengine, henum, UINT32_MAX, &matchingFwpFilter, &count);

		if (rc != ERROR_SUCCESS)
		{
			app.LogError (L"FwpmFilterEnum", rc, nullptr, 0);
		}
		else
		{
			if (matchingFwpFilter)
			{
				for (UINT32 i = 0; i < count; i++)
				{
					FWPM_FILTER* pfilter = matchingFwpFilter[i];

					if (pfilter && pfilter->providerKey && RtlEqualMemory (pfilter->providerKey, pprovider_id, sizeof (GUID)))
						ptr_filters->push_back (pfilter->filterKey);
				}

				FwpmFreeMemory ((void**)&matchingFwpFilter);
			}
			else
			{
				count = 0;
			}
		}
	}

	if (henum)
		FwpmFilterDestroyEnumHandle (hengine, henum);

	return count;
}

bool _mps_firewallapi (bool* pis_enabled, const bool* pis_enable)
{
	if (!pis_enabled && !pis_enable)
		return false;

	bool result = false;

	INetFwPolicy2* pNetFwPolicy2 = nullptr;
	HRESULT hr = CoCreateInstance (__uuidof (NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof (INetFwPolicy2), (void**)&pNetFwPolicy2);

	if (SUCCEEDED (hr) && pNetFwPolicy2)
	{
		const NET_FW_PROFILE_TYPE2 profileTypes[] = {
			NET_FW_PROFILE2_DOMAIN,
			NET_FW_PROFILE2_PRIVATE,
			NET_FW_PROFILE2_PUBLIC
		};

		if (pis_enabled)
		{
			*pis_enabled = false;

			for (auto &type : profileTypes)
			{
				VARIANT_BOOL bIsEnabled = FALSE;

				hr = pNetFwPolicy2->get_FirewallEnabled (type, &bIsEnabled);

				if (SUCCEEDED (hr))
				{
					result = true;

					if (bIsEnabled == VARIANT_TRUE)
					{
						*pis_enabled = true;
						break;
					}
				}
			}
		}

		if (pis_enable)
		{
			for (auto &type : profileTypes)
			{
				hr = pNetFwPolicy2->put_FirewallEnabled (type, *pis_enable ? VARIANT_TRUE : VARIANT_FALSE);

				if (SUCCEEDED (hr))
					result = true;

				else
					app.LogError (L"put_FirewallEnabled", hr, _r_fmt (L"%d", type), 0);
			}
		}
	}
	else
	{
		app.LogError (L"CoCreateInstance", hr, L"INetFwPolicy2", 0);
	}

	if (pNetFwPolicy2)
		pNetFwPolicy2->Release ();

	return result;
}

void _mps_changeconfig2 (bool is_enable)
{
	// check settings
	bool is_wfenabled = false;
	_mps_firewallapi (&is_wfenabled, nullptr);

	if (is_wfenabled == is_enable)
		return;

	const SC_HANDLE scm = OpenSCManager (nullptr, nullptr, SC_MANAGER_ALL_ACCESS);

	if (!scm)
	{
		app.LogError (L"OpenSCManager", GetLastError (), nullptr, 0);
	}
	else
	{
		LPCWSTR ServiceNames[] = {
			L"mpssvc",
			L"mpsdrv",
		};

		bool is_started = false;

		for (auto &name : ServiceNames)
		{
			const SC_HANDLE sc = OpenService (scm, name, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP);

			if (!sc)
			{
				const DWORD rc = GetLastError ();

				if (rc != ERROR_ACCESS_DENIED)
					app.LogError (L"OpenService", rc, name, 0);
			}
			else
			{
				if (!is_started)
				{
					SERVICE_STATUS status;

					if (QueryServiceStatus (sc, &status))
						is_started = (status.dwCurrentState == SERVICE_RUNNING);
				}

				if (!ChangeServiceConfig (sc, SERVICE_NO_CHANGE, is_enable ? SERVICE_AUTO_START : SERVICE_DISABLED, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
					app.LogError (L"ChangeServiceConfig", GetLastError (), name, 0);

				CloseServiceHandle (sc);
			}
		}

		// start services
		if (is_enable)
		{
			for (auto &name : ServiceNames)
			{
				SC_HANDLE sc = OpenService (scm, name, SERVICE_QUERY_STATUS | SERVICE_START);

				if (!sc)
				{
					app.LogError (L"OpenService", GetLastError (), name, 0);
				}
				else
				{
					DWORD dwBytesNeeded = 0;
					SERVICE_STATUS_PROCESS ssp = {0};

					if (!QueryServiceStatusEx (sc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof (ssp), &dwBytesNeeded))
					{
						app.LogError (L"QueryServiceStatusEx", GetLastError (), name, 0);
					}
					else
					{
						if (ssp.dwCurrentState != SERVICE_RUNNING)
						{
							if (!StartService (sc, 0, nullptr))
								app.LogError (L"StartService", GetLastError (), name, 0);
						}

						CloseServiceHandle (sc);
					}
				}
			}

			_r_sleep (250);
		}

		_mps_firewallapi (nullptr, &is_enable);

		CloseServiceHandle (scm);
	}
}

DWORD _FwpmGetAppIdFromFileName1 (LPCWSTR path, FWP_BYTE_BLOB** lpblob, EnumDataType type)
{
	if (_r_str_isempty (path) || !lpblob)
		return ERROR_BAD_ARGUMENTS;

	rstring path_buff;

	if (type == DataAppRegular || type == DataAppNetwork || type == DataAppService)
	{
		path_buff = path;

		if (_r_str_hash (path_buff) == config.ntoskrnl_hash)
		{
			if (ByteBlobAlloc ((LPVOID)path_buff.GetString (), (path_buff.GetLength () + 1) * sizeof (WCHAR), lpblob))
				return ERROR_SUCCESS;
		}
		else
		{
			DWORD rc = _r_path_ntpathfromdos (path_buff);

			// file is inaccessible or not found, maybe low-level driver preventing file access?
			// try another way!
			if (
				rc == ERROR_ACCESS_DENIED ||
				rc == ERROR_FILE_NOT_FOUND ||
				rc == ERROR_PATH_NOT_FOUND
				)
			{
				if (PathIsRelative (path))
				{
					return rc;
				}
				else
				{
					// file path (root)
					WCHAR path_root[MAX_PATH] = {0};
					_r_str_copy (path_root, _countof (path_root), path);
					PathStripToRoot (path_root);

					// file path (without root)
					WCHAR path_noroot[MAX_PATH] = {0};
					_r_str_copy (path_noroot, _countof (path_noroot), PathSkipRoot (path));

					path_buff = path_root;
					rc = _r_path_ntpathfromdos (path_buff);

					if (rc != ERROR_SUCCESS)
						return rc;

					path_buff.Append (path_noroot);

					_r_str_tolower (path_buff.GetBuffer ()); // lower is important!
				}
			}
			else if (rc != ERROR_SUCCESS)
			{
				return rc;
			}

			if (ByteBlobAlloc ((LPVOID)path_buff.GetString (), (path_buff.GetLength () + 1) * sizeof (WCHAR), lpblob))
				return ERROR_SUCCESS;
		}
	}
	else if (type == DataAppPico || type == DataAppDevice)
	{
		path_buff = path;

		if (type == DataAppDevice)
			_r_str_tolower (path_buff.GetBuffer ()); // lower is important!

		if (ByteBlobAlloc ((LPVOID)path_buff.GetString (), (path_buff.GetLength () + 1) * sizeof (WCHAR), lpblob))
			return ERROR_SUCCESS;
	}

	return ERROR_FILE_NOT_FOUND;
}

bool ByteBlobAlloc (const PVOID data, size_t length, FWP_BYTE_BLOB** lpblob)
{
	if (!data || !length || !lpblob)
		return false;

	FWP_BYTE_BLOB* pblob = (FWP_BYTE_BLOB*)_r_mem_allocex (sizeof (FWP_BYTE_BLOB), HEAP_ZERO_MEMORY);

	if (!pblob)
		return false;

	pblob->data = (UINT8*)_r_mem_alloc (length);
	pblob->size = (UINT32)length;

	RtlCopyMemory (pblob->data, data, length);

	*lpblob = pblob;

	return true;
}

void ByteBlobFree (FWP_BYTE_BLOB** lpblob)
{
	if (lpblob && *lpblob)
	{
		FWP_BYTE_BLOB* blob = *lpblob;

		if (blob)
		{
			_r_mem_free (blob->data);
			_r_mem_free (blob);

			*lpblob = nullptr;
		}
	}
}
