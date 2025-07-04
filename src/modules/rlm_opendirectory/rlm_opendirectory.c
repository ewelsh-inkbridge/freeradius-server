/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License, version 2 of the
 *   License as published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_opendirectory.c
 * @brief Allows authentication against OpenDirectory and enforces ACLS.
 *
 * authentication: Apple Open Directory authentication
 * authorization:  enforces ACLs
 *
 * @copyright 2007 Apple Inc.
 */

/*
 * 	For a typical Makefile, add linker flag like this:
 *	LDFLAGS = -framework DirectoryService
 */
USES_APPLE_DEPRECATED_API
#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/perm.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <DirectoryService/DirectoryService.h>
#include <membership.h>

typedef struct {
	fr_dict_enum_value_t const	*auth_type;
} rlm_opendirectory_t;

#ifndef HAVE_DECL_MBR_CHECK_SERVICE_MEMBERSHIP
int mbr_check_service_membership(uuid_t const user, char const *servicename, int *ismember);
#endif
#ifndef HAVE_DECL_MBR_CHECK_MEMBERSHIP_REFRESH
int mbr_check_membership_refresh(uuid_t const user, uuid_t group, int *ismember);
#endif

/* RADIUS service ACL constants */
#define kRadiusSACLName		"com.apple.access_radius"
#define kRadiusServiceName	"radius"

static fr_dict_t const *dict_freeradius;
static fr_dict_t const *dict_radius;

extern fr_dict_autoload_t rlm_opendirectory_dict[];
fr_dict_autoload_t rlm_opendirectory_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_auth_type;
static fr_dict_attr_t const *attr_user_name;
static fr_dict_attr_t const *attr_user_password;

extern fr_dict_attr_autoload_t rlm_opendirectory_dict_attr[];
fr_dict_attr_autoload_t rlm_opendirectory_dict_attr[] = {
	{ .out = &attr_auth_type, .name = "Auth-Type", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_user_name, .name = "User-Name", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_user_password, .name = "User-Password", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ NULL }
};

/*
 *	od_check_passwd
 *
 *  Returns: ds err
 */

static long od_check_passwd(request_t *request, char const *uname, char const *password)
{
	long			result 		= eDSAuthFailed;
	tDirReference		dsRef 		= 0;
	tDataBuffer		*tDataBuff;
	tDirNodeReference	nodeRef 	= 0;
	long			status 		= eDSNoErr;
	tContextData		context		= 0;
	uint32_t		nodeCount 	= 0;
	uint32_t		attrIndex 	= 0;
	tDataList		*nodeName 	= NULL;
	tAttributeEntryPtr	pAttrEntry 	= NULL;
	tDataList		*pRecName 	= NULL;
	tDataList		*pRecType 	= NULL;
	tDataList		*pAttrType 	= NULL;
	uint32_t		recCount 	= 0;
	tRecordEntry		*pRecEntry 	= NULL;
	tAttributeListRef	attrListRef 	= 0;
	char			*pUserLocation 	= NULL;
	char			*pUserName 	= NULL;
	tAttributeValueListRef	valueRef 	= 0;
	tAttributeValueEntry	*pValueEntry 	= NULL;
	tDataList		*pUserNode 	= NULL;
	tDirNodeReference	userNodeRef 	= 0;
	tDataBuffer		*pStepBuff 	= NULL;
	tDataNode		*pAuthType 	= NULL;
	tAttributeValueEntry	*pRecordType 	= NULL;
	uint32_t		uiCurr 		= 0;
	uint32_t		uiLen 		= 0;
	uint32_t		pwLen 		= 0;

	if (!uname || !password)
		return result;

	do
	{
		status = dsOpenDirService( &dsRef );
		if ( status != eDSNoErr )
			return result;

		tDataBuff = dsDataBufferAllocate( dsRef, 4096 );
		if (!tDataBuff)
			break;

		/* find user on search node */
		status = dsFindDirNodes( dsRef, tDataBuff, NULL, eDSSearchNodeName, &nodeCount, &context );
		if (status != eDSNoErr || nodeCount < 1)
			break;

		status = dsGetDirNodeName( dsRef, tDataBuff, 1, &nodeName );
		if (status != eDSNoErr)
			break;

		status = dsOpenDirNode( dsRef, nodeName, &nodeRef );
		dsDataListDeallocate( dsRef, nodeName );
		free( nodeName );
		nodeName = NULL;
		if (status != eDSNoErr)
			break;

		pRecName = dsBuildListFromStrings( dsRef, uname, NULL );
		pRecType = dsBuildListFromStrings( dsRef, kDSStdRecordTypeUsers, kDSStdRecordTypeComputers, kDSStdRecordTypeMachines, NULL );
		pAttrType = dsBuildListFromStrings( dsRef, kDSNAttrMetaNodeLocation, kDSNAttrRecordName, kDSNAttrRecordType, NULL );

		recCount = 1;
		status = dsGetRecordList( nodeRef, tDataBuff, pRecName, eDSExact, pRecType,
													pAttrType, 0, &recCount, &context );
		if ( status != eDSNoErr || recCount == 0 )
			break;

		status = dsGetRecordEntry( nodeRef, tDataBuff, 1, &attrListRef, &pRecEntry );
		if ( status != eDSNoErr )
			break;

		for ( attrIndex = 1; (attrIndex <= pRecEntry->fRecordAttributeCount) && (status == eDSNoErr); attrIndex++ )
		{
			status = dsGetAttributeEntry( nodeRef, tDataBuff, attrListRef, attrIndex, &valueRef, &pAttrEntry );
			if ( status == eDSNoErr && pAttrEntry != NULL )
			{
				if ( strcmp( pAttrEntry->fAttributeSignature.fBufferData, kDSNAttrMetaNodeLocation ) == 0 )
				{
					status = dsGetAttributeValue( nodeRef, tDataBuff, 1, valueRef, &pValueEntry );
					if ( status == eDSNoErr && pValueEntry != NULL )
					{
						pUserLocation = talloc_zero_array(request, char, pValueEntry->fAttributeValueData.fBufferLength + 1);
						memcpy( pUserLocation, pValueEntry->fAttributeValueData.fBufferData, pValueEntry->fAttributeValueData.fBufferLength );
					}
				}
				else
				if ( strcmp( pAttrEntry->fAttributeSignature.fBufferData, kDSNAttrRecordName ) == 0 )
				{
					status = dsGetAttributeValue( nodeRef, tDataBuff, 1, valueRef, &pValueEntry );
					if ( status == eDSNoErr && pValueEntry != NULL )
					{
						pUserName = talloc_zero_array(request, char, pValueEntry->fAttributeValueData.fBufferLength + 1);
						memcpy( pUserName, pValueEntry->fAttributeValueData.fBufferData, pValueEntry->fAttributeValueData.fBufferLength );
					}
				}
				else
				if ( strcmp( pAttrEntry->fAttributeSignature.fBufferData, kDSNAttrRecordType ) == 0 )
				{
					status = dsGetAttributeValue( nodeRef, tDataBuff, 1, valueRef, &pValueEntry );
					if ( status == eDSNoErr && pValueEntry != NULL )
					{
						pRecordType = pValueEntry;
						pValueEntry = NULL;
					}
				}

				if ( pValueEntry != NULL ) {
					dsDeallocAttributeValueEntry( dsRef, pValueEntry );
					pValueEntry = NULL;
				}
				if ( pAttrEntry != NULL ) {
					dsDeallocAttributeEntry( dsRef, pAttrEntry );
					pAttrEntry = NULL;
				}
				dsCloseAttributeValueList( valueRef );
				valueRef = 0;
			}
		}

		pUserNode = dsBuildFromPath( dsRef, pUserLocation, "/" );
		status = dsOpenDirNode( dsRef, pUserNode, &userNodeRef );
		dsDataListDeallocate( dsRef, pUserNode );
		free( pUserNode );
		pUserNode = NULL;
		if ( status != eDSNoErr )
			break;

		pStepBuff = dsDataBufferAllocate( dsRef, 128 );

		pAuthType = dsDataNodeAllocateString( dsRef, kDSStdAuthNodeNativeClearTextOK );
		uiCurr = 0;

		if (!pUserName) {
			RDEBUG2("Failed to find user name");
			break;
		}

		/* User name */
		uiLen = (uint32_t)strlen( pUserName );
		memcpy( &(tDataBuff->fBufferData[ uiCurr ]), &uiLen, sizeof(uiLen) );
		uiCurr += (uint32_t)sizeof( uiLen );
		memcpy( &(tDataBuff->fBufferData[ uiCurr ]), pUserName, uiLen );
		uiCurr += uiLen;

		/* pw */
		pwLen = (uint32_t)strlen( password );
		memcpy( &(tDataBuff->fBufferData[ uiCurr ]), &pwLen, sizeof(pwLen) );
		uiCurr += (uint32_t)sizeof( pwLen );
		memcpy( &(tDataBuff->fBufferData[ uiCurr ]), password, pwLen );
		uiCurr += pwLen;

		tDataBuff->fBufferLength = uiCurr;

		result = dsDoDirNodeAuthOnRecordType( userNodeRef, pAuthType, 1, tDataBuff, pStepBuff, NULL, &pRecordType->fAttributeValueData );
	}
	while ( 0 );

	/* clean up */
	if (pAuthType != NULL) {
		dsDataNodeDeAllocate( dsRef, pAuthType );
		pAuthType = NULL;
	}
	if (pRecordType != NULL) {
		dsDeallocAttributeValueEntry( dsRef, pRecordType );
		pRecordType = NULL;
	}
	if (tDataBuff != NULL) {
		bzero( tDataBuff, tDataBuff->fBufferSize );
		dsDataBufferDeAllocate( dsRef, tDataBuff );
		tDataBuff = NULL;
	}
	if (pStepBuff != NULL) {
		dsDataBufferDeAllocate( dsRef, pStepBuff );
		pStepBuff = NULL;
	}
	if (pUserLocation != NULL) {
		talloc_free(pUserLocation);
		pUserLocation = NULL;
	}
	if (pUserName != NULL) {
		talloc_free(pUserName);
		pUserName = NULL;
	}
	if (pRecName != NULL) {
		dsDataListDeallocate( dsRef, pRecName );
		free( pRecName );
		pRecName = NULL;
	}
	if (pRecType != NULL) {
		dsDataListDeallocate( dsRef, pRecType );
		free( pRecType );
		pRecType = NULL;
	}
	if (pAttrType != NULL) {
		dsDataListDeallocate( dsRef, pAttrType );
		free( pAttrType );
		pAttrType = NULL;
	}
	if (nodeRef != 0) {
		dsCloseDirNode(nodeRef);
		nodeRef = 0;
	}
	if (dsRef != 0) {
		dsCloseDirService(dsRef);
		dsRef = 0;
	}

	return result;
}


/*
 *	Check the users password against the standard UNIX
 *	password table.
 */
static unlang_action_t CC_HINT(nonnull) mod_authenticate(unlang_result_t *p_result, UNUSED module_ctx_t const *mctx, request_t *request)
{
	int		ret;
	long		odResult = eDSAuthFailed;
	fr_pair_t *username, *password;

	username = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_name);
	password = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_password);

	/*
	 *	We can only authenticate user requests which HAVE
	 *	a User-Name attribute.
	 */
	if (!username) {
		REDEBUG("Attribute \"User-Name\" is required for authentication");
		RETURN_UNLANG_INVALID;
	}

	if (!password) {
		REDEBUG("Attribute \"User-Password\" is required for authentication");
		RETURN_UNLANG_INVALID;
	}

	/*
	 *	Make sure the supplied password isn't empty
	 */
	if (password->vp_length == 0) {
		REDEBUG("User-Password must not be empty");
		RETURN_UNLANG_INVALID;
	}

	/*
	 *	Log the password
	 */
	if (RDEBUG_ENABLED3) {
		RDEBUG("Login attempt with password \"%pV\"", &password->data);
	} else {
		RDEBUG2("Login attempt with password");
	}

	odResult = od_check_passwd(request, username->vp_strvalue,
				   password->vp_strvalue);
	switch (odResult) {
		case eDSNoErr:
			ret = RLM_MODULE_OK;
			break;

		case eDSAuthUnknownUser:
		case eDSAuthInvalidUserName:
		case eDSAuthNewPasswordRequired:
		case eDSAuthPasswordExpired:
		case eDSAuthAccountDisabled:
		case eDSAuthAccountExpired:
		case eDSAuthAccountInactive:
		case eDSAuthInvalidLogonHours:
		case eDSAuthInvalidComputer:
			ret = RLM_MODULE_DISALLOW;
			break;

		default:
			ret = RLM_MODULE_REJECT;
			break;
	}

	if (ret != RLM_MODULE_OK) {
		RDEBUG2("Invalid password: %pV", &username->data);
		return ret;
	}

	RETURN_UNLANG_OK;
}


/*
 *	member of the radius group?
 */
static unlang_action_t CC_HINT(nonnull) mod_authorize(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_opendirectory_t const	*inst = talloc_get_type_abort_const(mctx->mi->data, rlm_opendirectory_t);
	struct passwd			*userdata = NULL;
	int				ismember = 0;
	fr_client_t			*client = NULL;
	uuid_t				uuid;
	uuid_t				guid_sacl;
	uuid_t				guid_nasgroup;
	int				err;
	char				host_ipaddr[128] = {0};
	gid_t				gid;
	fr_pair_t			*username;

	/*
	 *	We can only authenticate user requests which HAVE
	 *	a User-Name attribute.
	 */
	username = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_name);
	if (!username) {
		RDEBUG2("OpenDirectory requires a User-Name attribute");
		RETURN_UNLANG_NOOP;
	}

	/* resolve SACL */
	uuid_clear(guid_sacl);

	if (fr_perm_gid_from_str(request, &gid, kRadiusSACLName) < 0) {
		RDEBUG2("The SACL group \"%s\" does not exist on this system", kRadiusSACLName);
	} else {
		err = mbr_gid_to_uuid(gid, guid_sacl);
		if (err != 0) {
			REDEBUG("The group \"%s\" does not have a GUID", kRadiusSACLName);
			RETURN_UNLANG_FAIL;
		}
	}

	/* resolve client access list */
	uuid_clear(guid_nasgroup);

	client = client_from_request(request);
#if 0
	if (client->community[0] != '\0' ) {
		/*
		 *	The "community" can be a GUID (Globally Unique ID) or
		 *	a group name
		 */
		if (uuid_parse(client->community, guid_nasgroup) != 0) {
			/* attempt to resolve the name */
			groupdata = getgrnam(client->community);
			if (!groupdata) {
				REDEBUG("The group \"%s\" does not exist on this system", client->community);
				RETURN_UNLANG_FAIL;
			}
			err = mbr_gid_to_uuid(groupdata->gr_gid, guid_nasgroup);
			if (err != 0) {
				REDEBUG("The group \"%s\" does not have a GUID", client->community);
				RETURN_UNLANG_FAIL;
			}
		}
	}
	else
#endif
	{
		if (!client) {
			RDEBUG2("The client record could not be found for host %s",
			       fr_inet_ntoh(&request->packet->socket.inet.src_ipaddr, host_ipaddr, sizeof(host_ipaddr)));
		} else {
			RDEBUG2("The host %s does not have an access group",
			       fr_inet_ntoh(&request->packet->socket.inet.src_ipaddr, host_ipaddr, sizeof(host_ipaddr)));
		}
	}

	if (uuid_is_null(guid_sacl) && uuid_is_null(guid_nasgroup)) {
		RDEBUG2("No access control groups, all users allowed");
		goto setup_auth_type;
	}

	/* resolve user */
	uuid_clear(uuid);

	fr_perm_getpwnam(request, &userdata, username->vp_strvalue);
	if (userdata != NULL) {
		err = mbr_uid_to_uuid(userdata->pw_uid, uuid);
		if (err != 0)
			uuid_clear(uuid);
	}
	talloc_free(userdata);

	if (uuid_is_null(uuid)) {
		REDEBUG("Could not get the user's uuid");
		RETURN_UNLANG_NOTFOUND;
	}

	if (!uuid_is_null(guid_sacl)) {
		err = mbr_check_service_membership(uuid, kRadiusServiceName, &ismember);
		if (err != 0) {
			REDEBUG("Failed to check group membership");
			RETURN_UNLANG_FAIL;
		}

		if (ismember == 0) {
			REDEBUG("User is not authorized");
			RETURN_UNLANG_DISALLOW;
		}
	}

	if (!uuid_is_null(guid_nasgroup)) {
		err = mbr_check_membership_refresh(uuid, guid_nasgroup, &ismember);
		if (err != 0) {
			REDEBUG("Failed to check group membership");
			RETURN_UNLANG_FAIL;
		}

		if (ismember == 0) {
			REDEBUG("User is not authorized");
			RETURN_UNLANG_DISALLOW;
		}
	}

setup_auth_type:
	if (!inst->auth_type) {
		WARN("No 'authenticate %s {...}' section or 'Auth-Type = %s' set.  Cannot setup OpenDirectory authentication",
		     mctx->mi->name, mctx->mi->name);
		RETURN_UNLANG_NOOP;
	}

	if (!module_rlm_section_type_set(request, attr_auth_type, inst->auth_type)) RETURN_UNLANG_NOOP;

	RETURN_UNLANG_OK;
}

static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_opendirectory_t *inst = talloc_get_type_abort(mctx->mi->data, rlm_opendirectory_t);

	inst->auth_type = fr_dict_enum_by_name(attr_auth_type, mctx->mi->name, -1);
	if (!inst->auth_type) {
		WARN("Failed to find 'authenticate %s {...}' section.  OpenDirectory authentication will likely not work",
		     mctx->mi->name);
	}

	return 0;
}

/* globally exported name */
extern module_rlm_t rlm_opendirectory;
module_rlm_t rlm_opendirectory = {
	.common = {
		.magic		= MODULE_MAGIC_INIT,
		.name		= "opendirectory",
		.inst_size	= sizeof(rlm_opendirectory_t),
		.instantiate	= mod_instantiate
	},
	.method_group = {
		.bindings = (module_method_binding_t[]){
			{ .section = SECTION_NAME("authenticate", CF_IDENT_ANY), .method = mod_authenticate },
			{ .section = SECTION_NAME("recv", CF_IDENT_ANY), .method = mod_authorize },
			MODULE_BINDING_TERMINATOR
		}
	}
};
