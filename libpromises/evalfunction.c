/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <evalfunction.h>

#include <env_context.h>
#include <promises.h>
#include <dir.h>
#include <dbm_api.h>
#include <lastseen.h>
#include <files_names.h>
#include <files_interfaces.h>
#include <files_hashes.h>
#include <vars.h>
#include <addr_lib.h>
#include <syntax.h>
#include <item_lib.h>
#include <conversion.h>
#include <expand.h>
#include <scope.h>
#include <keyring.h>
#include <matching.h>
#include <hashes.h>
#include <unix.h>
#include <string_lib.h>
#include <args.h>
#include <client_code.h>
#include <communication.h>
#include <classic.h>                                    /* SendSocketStream */
#include <pipes.h>
#include <exec_tools.h>
#include <policy.h>
#include <misc_lib.h>
#include <fncall.h>
#include <audit.h>
#include <sort.h>
#include <logging.h>
#include <set.h>
#include <buffer.h>
#include <files_lib.h>

#include <math_eval.h>

#include <libgen.h>

#ifndef __MINGW32__
#include <glob.h>
#endif

#include <ctype.h>


/*
 * This module contains numeruous functions which don't use all their parameters
 * (e.g. language-function calls which don't use EvalContext or
 * language-function calls which don't use arguments as language-function does
 * not accept any).
 *
 * Temporarily, in order to avoid cluttering output with thousands of warnings,
 * this module is excempted from producing warnings about unused function
 * parameters.
 *
 * Please remove this #pragma ASAP and provide ARG_UNUSED declarations for
 * unused parameters.
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

typedef enum
{
    DATE_TEMPLATE_YEAR,
    DATE_TEMPLATE_MONTH,
    DATE_TEMPLATE_DAY,
    DATE_TEMPLATE_HOUR,
    DATE_TEMPLATE_MIN,
    DATE_TEMPLATE_SEC
} DateTemplate;

static FnCallResult FilterInternal(EvalContext *ctx, FnCall *fp, char *regex, char *name, int do_regex, int invert, long max);

static char *StripPatterns(EvalContext *ctx, char *file_buffer, char *pattern, char *filename);
static void CloseStringHole(char *s, int start, int end);
static int BuildLineArray(EvalContext *ctx, const Bundle *bundle, char *array_lval, char *file_buffer, char *split, int maxent, DataType type, int intIndex);
static int ExecModule(EvalContext *ctx, char *command, const char *ns);
static int CheckID(char *id);
static bool GetListReferenceArgument(const EvalContext *ctx, const FnCall *fp, const char *lval_str, Rval *rval_out, DataType *datatype_out);
static void *CfReadFile(char *filename, int maxsize);

/*******************************************************************/

int FnNumArgs(const FnCallType *call_type)
{
    for (int i = 0;; i++)
    {
        if (call_type->args[i].pattern == NULL)
        {
            return i;
        }
    }
}

/*******************************************************************/

/* assume args are all scalar literals by the time we get here
     and each handler allocates the memory it returns. There is
     a protocol to be followed here:
     Set args,
     Eval Content,
     Set rtype,
     ErrorFlags

     returnval = FnCallXXXResult(fp)

  */

/*******************************************************************/
/* End FnCall API                                                  */
/*******************************************************************/

static Rlist *GetHostsFromLastseenDB(Item *addresses, time_t horizon, bool return_address, bool return_recent)
{
    Rlist *recent = NULL, *aged = NULL;
    Item *ip;
    time_t now = time(NULL);
    double entrytime;
    char address[CF_MAXVARSIZE];

    for (ip = addresses; ip != NULL; ip = ip->next)
    {
        if (sscanf(ip->classes, "%lf", &entrytime) != 1)
        {
            Log(LOG_LEVEL_ERR, "Could not get host entry age");
            continue;
        }

        if (return_address)
        {
            snprintf(address, sizeof(address), "%s", ip->name);
        }
        else
        {
            char hostname[MAXHOSTNAMELEN];
            if (IPString2Hostname(hostname, ip->name, sizeof(hostname)) != -1)
            {
                snprintf(address, sizeof(address), "%s", hostname);
            }
            else
            {
                /* Not numeric address was requested, but IP was unresolvable. */
                snprintf(address, sizeof(address), "%s", ip->name);
            }
        }

        if (entrytime < now - horizon)
        {
            Log(LOG_LEVEL_DEBUG, "Old entry");

            if (RlistKeyIn(recent, address))
            {
                Log(LOG_LEVEL_DEBUG, "There is recent entry for this address. Do nothing.");
            }
            else
            {
                Log(LOG_LEVEL_DEBUG, "Adding to list of aged hosts.");
                RlistPrependScalarIdemp(&aged, address);
            }
        }
        else
        {
            Rlist *r;

            Log(LOG_LEVEL_DEBUG, "Recent entry");

            if ((r = RlistKeyIn(aged, address)))
            {
                Log(LOG_LEVEL_DEBUG, "Purging from list of aged hosts.");
                RlistDestroyEntry(&aged, r);
            }

            Log(LOG_LEVEL_DEBUG, "Adding to list of recent hosts.");
            RlistPrependScalarIdemp(&recent, address);
        }
    }

    if (return_recent)
    {
        RlistDestroy(aged);
        if (recent == NULL)
        {
            RlistAppendScalarIdemp(&recent, CF_NULL_VALUE);
        }
        return recent;
    }
    else
    {
        RlistDestroy(recent);
        if (aged == NULL)
        {
            RlistAppendScalarIdemp(&aged, CF_NULL_VALUE);
        }
        return aged;
    }
}

/*********************************************************************/

static FnCallResult FnCallAnd(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *arg;
    char id[CF_BUFSIZE];

    snprintf(id, CF_BUFSIZE, "built-in FnCall and-arg");

/* We need to check all the arguments, ArgTemplate does not check varadic functions */
    for (arg = finalargs; arg; arg = arg->next)
    {
        SyntaxTypeMatch err = CheckConstraintTypeMatch(id, arg->val, DATA_TYPE_STRING, "", 1);
        if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
        {
            FatalError(ctx, "in %s: %s", id, SyntaxTypeMatchToString(err));
        }
    }

    for (arg = finalargs; arg; arg = arg->next)
    {
        if (!IsDefinedClass(ctx, RlistScalarValue(arg), PromiseGetNamespace(fp->caller)))
        {
            return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
        }
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
}

/*******************************************************************/

static bool CallHostsSeenCallback(const char *hostkey, const char *address,
                                  bool incoming, const KeyHostSeen *quality,
                                  void *ctx)
{
    Item **addresses = ctx;

    if (HostKeyAddressUnknown(hostkey))
    {
        return true;
    }

    char buf[CF_BUFSIZE];
    snprintf(buf, sizeof(buf), "%ju", (uintmax_t)quality->lastseen);

    PrependItem(addresses, address, buf);

    return true;
}

/*******************************************************************/

static FnCallResult FnCallHostsSeen(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Item *addresses = NULL;

    int horizon = IntFromString(RlistScalarValue(finalargs)) * 3600;
    char *policy = RlistScalarValue(finalargs->next);
    char *format = RlistScalarValue(finalargs->next->next);

    Log(LOG_LEVEL_DEBUG, "Calling hostsseen(%d,%s,%s)", horizon, policy, format);

    if (!ScanLastSeenQuality(&CallHostsSeenCallback, &addresses))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    Rlist *returnlist = GetHostsFromLastseenDB(addresses, horizon,
                                               strcmp(format, "address") == 0,
                                               strcmp(policy, "lastseen") == 0);

    DeleteItemList(addresses);

    {
        Writer *w = StringWriter();
        WriterWrite(w, "hostsseen return values:");
        for (Rlist *rp = returnlist; rp; rp = rp->next)
        {
            WriterWriteF(w, " '%s'", RlistScalarValue(rp));
        }
        Log(LOG_LEVEL_DEBUG, "%s", StringWriterData(w));
        WriterClose(w);
    }

    if (returnlist == NULL)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
    else
    {
        return (FnCallResult) { FNCALL_SUCCESS, { returnlist, RVAL_TYPE_LIST } };
    }
}

/*********************************************************************/

static FnCallResult FnCallHostsWithClass(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *returnlist = NULL;

    char *class_name = RlistScalarValue(finalargs);
    char *return_format = RlistScalarValue(finalargs->next);
    
    if(!ListHostsWithClass(ctx, &returnlist, class_name, return_format))
    {
        return (FnCallResult){ FNCALL_FAILURE };
    }
    
    return (FnCallResult) { FNCALL_SUCCESS, { returnlist, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallRandomInt(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];
    int tmp, range, result;

    buffer[0] = '\0';

/* begin fn specific content */

    int from = IntFromString(RlistScalarValue(finalargs));
    int to = IntFromString(RlistScalarValue(finalargs->next));

    if (from == CF_NOINT || to == CF_NOINT)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (from > to)
    {
        tmp = to;
        to = from;
        from = tmp;
    }

    range = fabs(to - from);
    result = from + (int) (drand48() * (double) range);
    snprintf(buffer, CF_BUFSIZE - 1, "%d", result);

/* end fn specific content */

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallGetEnv(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE] = "", ctrlstr[CF_SMALLBUF];

/* begin fn specific content */

    char *name = RlistScalarValue(finalargs);
    int limit = IntFromString(RlistScalarValue(finalargs->next));

    snprintf(ctrlstr, CF_SMALLBUF, "%%.%ds", limit);    // -> %45s

    if (getenv(name))
    {
        snprintf(buffer, CF_BUFSIZE - 1, ctrlstr, getenv(name));
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

#if defined(HAVE_GETPWENT)

static FnCallResult FnCallGetUsers(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *newlist = NULL, *except_names, *except_uids;
    struct passwd *pw;

/* begin fn specific content */

    char *except_name = RlistScalarValue(finalargs);
    char *except_uid = RlistScalarValue(finalargs->next);

    except_names = RlistFromSplitString(except_name, ',');
    except_uids = RlistFromSplitString(except_uid, ',');

    setpwent();

    while ((pw = getpwent()))
    {
        char *pw_uid_str = StringFromLong((int)pw->pw_uid);

        if (!RlistKeyIn(except_names, pw->pw_name) && !RlistKeyIn(except_uids, pw_uid_str))
        {
            RlistAppendScalarIdemp(&newlist, pw->pw_name);
        }

        free(pw_uid_str);
    }

    endpwent();

    return (FnCallResult) { FNCALL_SUCCESS, { newlist, RVAL_TYPE_LIST } };
}

#else

static FnCallResult FnCallGetUsers(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Log(LOG_LEVEL_ERR, "getusers is not implemented");
    return (FnCallResult) { FNCALL_FAILURE };
}

#endif

/*********************************************************************/

static FnCallResult FnCallEscape(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];

    buffer[0] = '\0';

/* begin fn specific content */

    char *name = RlistScalarValue(finalargs);

    EscapeSpecialChars(name, buffer, CF_BUFSIZE - 1, "", "");

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallHost2IP(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char *name = RlistScalarValue(finalargs);
    char ipaddr[CF_MAX_IP_LEN];

    if (Hostname2IPString(ipaddr, name, sizeof(ipaddr)) != -1)
    {
        return (FnCallResult) {
            FNCALL_SUCCESS, { xstrdup(ipaddr), RVAL_TYPE_SCALAR }
        };
    }
    else
    {
        /* Retain legacy behaviour,
           return hostname in case resolution fails. */
        return (FnCallResult) {
            FNCALL_SUCCESS, { xstrdup(name), RVAL_TYPE_SCALAR }
        };
    }

}

/*********************************************************************/

static FnCallResult FnCallIP2Host(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char hostname[MAXHOSTNAMELEN];
    char *ip = RlistScalarValue(finalargs);

    if (IPString2Hostname(hostname, ip, sizeof(hostname)) != -1)
    {
        return (FnCallResult) {
            FNCALL_SUCCESS, { xstrdup(hostname), RVAL_TYPE_SCALAR }
        };
    }
    else
    {
        /* Retain legacy behaviour,
           return ip address in case resolution fails. */
        return (FnCallResult) {
            FNCALL_SUCCESS, { xstrdup(ip), RVAL_TYPE_SCALAR }
        };
    }
}

/*********************************************************************/

#ifdef __MINGW32__

static FnCallResult FnCallGetUid(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    return (FnCallResult) { FNCALL_FAILURE };
}

#else /* !__MINGW32__ */

static FnCallResult FnCallGetUid(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    struct passwd *pw;

/* begin fn specific content */

    if ((pw = getpwnam(RlistScalarValue(finalargs))) == NULL)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
    else
    {
        char buffer[CF_BUFSIZE];

        snprintf(buffer, CF_BUFSIZE - 1, "%ju", (uintmax_t)pw->pw_uid);
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
    }
}

#endif /* !__MINGW32__ */

/*********************************************************************/

#ifdef __MINGW32__

static FnCallResult FnCallGetGid(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    return (FnCallResult) { FNCALL_FAILURE };
}

#else /* !__MINGW32__ */

static FnCallResult FnCallGetGid(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    struct group *gr;

/* begin fn specific content */

    if ((gr = getgrnam(RlistScalarValue(finalargs))) == NULL)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
    else
    {
        char buffer[CF_BUFSIZE];

        snprintf(buffer, CF_BUFSIZE - 1, "%ju", (uintmax_t)gr->gr_gid);
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
    }
}

#endif /* __MINGW32__ */

/*********************************************************************/

static FnCallResult FnCallHandlerHash(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
/* Hash(string,md5|sha1|crypt) */
{
    char buffer[CF_BUFSIZE];
    unsigned char digest[EVP_MAX_MD_SIZE + 1];
    HashMethod type;

    buffer[0] = '\0';

/* begin fn specific content */

    char *string = RlistScalarValue(finalargs);
    char *typestring = RlistScalarValue(finalargs->next);

    type = HashMethodFromString(typestring);

    if (FIPS_MODE && type == HASH_METHOD_MD5)
    {
        Log(LOG_LEVEL_ERR, "FIPS mode is enabled, and md5 is not an approved algorithm in call to hash()");
    }

    HashString(string, strlen(string), digest, type);

    char hashbuffer[EVP_MAX_MD_SIZE * 4];

    snprintf(buffer, CF_BUFSIZE - 1, "%s", HashPrintSafe(type, digest, hashbuffer));

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(SkipHashType(buffer)), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallHashMatch(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
/* HashMatch(string,md5|sha1|crypt,"abdxy98edj") */
{
    char buffer[CF_BUFSIZE], ret[CF_BUFSIZE];
    unsigned char digest[EVP_MAX_MD_SIZE + 1];
    HashMethod type;

    buffer[0] = '\0';

/* begin fn specific content */

    char *string = RlistScalarValue(finalargs);
    char *typestring = RlistScalarValue(finalargs->next);
    char *compare = RlistScalarValue(finalargs->next->next);

    type = HashMethodFromString(typestring);
    HashFile(string, digest, type);

    char hashbuffer[EVP_MAX_MD_SIZE * 4];
    snprintf(buffer, CF_BUFSIZE - 1, "%s", HashPrintSafe(type, digest, hashbuffer));
    Log(LOG_LEVEL_VERBOSE, "File '%s' hashes to '%s', compare to '%s'", string, buffer, compare);

    if (strcmp(buffer + 4, compare) == 0)
    {
        strcpy(ret, "any");
    }
    else
    {
        strcpy(ret, "!any");
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(ret), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallConcat(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *arg = NULL;
    char id[CF_BUFSIZE];
    char result[CF_BUFSIZE] = "";

    snprintf(id, CF_BUFSIZE, "built-in FnCall concat-arg");

/* We need to check all the arguments, ArgTemplate does not check varadic functions */
    for (arg = finalargs; arg; arg = arg->next)
    {
        SyntaxTypeMatch err = CheckConstraintTypeMatch(id, arg->val, DATA_TYPE_STRING, "", 1);
        if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
        {
            FatalError(ctx, "in %s: %s", id, SyntaxTypeMatchToString(err));
        }
    }

    for (arg = finalargs; arg; arg = arg->next)
    {
        if (strlcat(result, RlistScalarValue(arg), CF_BUFSIZE) >= CF_BUFSIZE)
        {
            /* Complain */
            Log(LOG_LEVEL_ERR, "Unable to evaluate concat() function, arguments are too long");
            return (FnCallResult) { FNCALL_FAILURE};
        }
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(result), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallClassMatch(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    const char *regex = RlistScalarValue(finalargs);
    {
        ClassTableIterator *iter = EvalContextClassTableIteratorNewGlobal(ctx, NULL, true, true);
        Class *cls = NULL;
        while ((cls = ClassTableIteratorNext(iter)))
        {
            char *expr = ClassRefToString(cls->ns, cls->name);

            if (StringMatchFull(regex, expr))
            {
                free(expr);
                return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
            }

            free(expr);
        }
        ClassTableIteratorDestroy(iter);
    }

    {
        ClassTableIterator *iter = EvalContextClassTableIteratorNewLocal(ctx);
        Class *cls = NULL;
        while ((cls = ClassTableIteratorNext(iter)))
        {
            char *expr = ClassRefToString(cls->ns, cls->name);

            if (StringMatchFull(regex, expr))
            {
                free(expr);
                return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
            }

            free(expr);
        }
        ClassTableIteratorDestroy(iter);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallIfElse(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *arg = NULL;
    int argcount = 0;
    char id[CF_BUFSIZE];

    snprintf(id, CF_BUFSIZE, "built-in FnCall ifelse-arg");

    /* We need to check all the arguments, ArgTemplate does not check varadic functions */
    for (arg = finalargs; arg; arg = arg->next)
    {
        SyntaxTypeMatch err = CheckConstraintTypeMatch(id, arg->val, DATA_TYPE_STRING, "", 1);
        if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
        {
            FatalError(ctx, "in %s: %s", id, SyntaxTypeMatchToString(err));
        }
        argcount++;
    }

    /* Require an odd number of arguments. We will always return something. */
    if (argcount%2 != 1)
    {
        FatalError(ctx, "in built-in FnCall ifelse: even number of arguments");
    }

    for (arg = finalargs;        /* Start with arg set to finalargs. */
         arg && arg->next;       /* We must have arg and arg->next to proceed. */
         arg = arg->next->next)  /* arg steps forward *twice* every time. */
    {
        /* Similar to classmatch(), we evaluate the first of the two
         * arguments as a class. */
        if (IsDefinedClass(ctx, RlistScalarValue(arg), PromiseGetNamespace(fp->caller)))
        {
            /* If the evaluation returned true in the current context,
             * return the second of the two arguments. */
            return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(RlistScalarValue(arg->next)), RVAL_TYPE_SCALAR } };
        }
    }

    /* If we get here, we've reached the last argument (arg->next is NULL). */
    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(RlistScalarValue(arg)), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallCountClassesMatching(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    unsigned count = 0;
    const char *regex = RlistScalarValue(finalargs);
    {
        ClassTableIterator *iter = EvalContextClassTableIteratorNewGlobal(ctx, NULL, true, true);
        Class *cls = NULL;
        while ((cls = ClassTableIteratorNext(iter)))
        {
            char *expr = ClassRefToString(cls->ns, cls->name);

            if (StringMatchFull(regex, expr))
            {
                count++;
            }

            free(expr);
        }
        ClassTableIteratorDestroy(iter);
    }

    {
        ClassTableIterator *iter = EvalContextClassTableIteratorNewLocal(ctx);
        Class *cls = NULL;
        while ((cls = ClassTableIteratorNext(iter)))
        {
            char *expr = ClassRefToString(cls->ns, cls->name);

            if (StringMatchFull(regex, expr))
            {
                count++;
            }

            free(expr);
        }
        ClassTableIteratorDestroy(iter);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { StringFromLong(count), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static StringSet *ClassesMatching(const EvalContext *ctx, ClassTableIterator *iter, const Rlist *args)
{
    StringSet *matching = StringSetNew();

    const char *regex = RlistScalarValue(args);
    Class *cls = NULL;
    while ((cls = ClassTableIteratorNext(iter)))
    {
        char *expr = ClassRefToString(cls->ns, cls->name);

        if (StringMatchFull(regex, expr))
        {
            bool pass = true;
            StringSet *tagset = EvalContextClassTags(ctx, cls->ns, cls->name);
            for (const Rlist *arg = args->next; (pass && arg); arg = arg->next)
            {
                const char *tag_regex = RlistScalarValue(arg);
                const char *element = NULL;
                StringSetIterator it = StringSetIteratorInit(tagset);
                while ((element = StringSetIteratorNext(&it)))
                {
                    if (!StringMatchFull(tag_regex, element))
                    {
                        pass = false;
                    }
                }
            }

            if (pass)
            {
                StringSetAdd(matching, expr);
            }
        }
        else
        {
            free(expr);
        }
    }

    return matching;
}

static FnCallResult FnCallClassesMatching(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    if (!finalargs)
    {
        FatalError(ctx, "Function '%s' requires at least one argument", fp->name);
    }

    for (const Rlist *arg = finalargs; arg; arg = arg->next)
    {
        SyntaxTypeMatch err = CheckConstraintTypeMatch(fp->name, arg->val, DATA_TYPE_STRING, "", 1);
        if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
        {
            FatalError(ctx, "in function '%s', '%s'", fp->name, SyntaxTypeMatchToString(err));
        }
    }

    Rlist *matches = NULL;

    {
        ClassTableIterator *iter = EvalContextClassTableIteratorNewGlobal(ctx, PromiseGetNamespace(fp->caller), true, true);
        StringSet *global_matches = ClassesMatching(ctx, iter, finalargs);

        StringSetIterator it = StringSetIteratorInit(global_matches);
        const char *element = NULL;
        while ((element = StringSetIteratorNext(&it)))
        {
            RlistPrepend(&matches, element, RVAL_TYPE_SCALAR);
        }

        StringSetDestroy(global_matches);
        ClassTableIteratorDestroy(iter);
    }

    {
        ClassTableIterator *iter = EvalContextClassTableIteratorNewLocal(ctx);
        StringSet *local_matches = ClassesMatching(ctx, iter, finalargs);

        StringSetIterator it = StringSetIteratorInit(local_matches);
        const char *element = NULL;
        while ((element = StringSetIteratorNext(&it)))
        {
            RlistPrepend(&matches, element, RVAL_TYPE_SCALAR);
        }

        StringSetDestroy(local_matches);
        ClassTableIteratorDestroy(iter);
    }

    if (!matches)
    {
        RlistAppendScalarIdemp(&matches, CF_NULL_VALUE);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { matches, RVAL_TYPE_LIST } };
}


static StringSet *VariablesMatching(const EvalContext *ctx, VariableTableIterator *iter, const Rlist *args)
{
    StringSet *matching = StringSetNew();

    const char *regex = RlistScalarValue(args);
    Variable *v = NULL;
    while ((v = VariableTableIteratorNext(iter)))
    {
        char *expr = VarRefToString(v->ref, true);

        if (StringMatchFull(regex, expr))
        {
            bool pass = true;
            StringSet *tagset = EvalContextVariableTags(ctx, v->ref);
            for (const Rlist *arg = args->next; (pass && arg); arg = arg->next)
            {
                const char* tag_regex = RlistScalarValue(arg);
                const char *element = NULL;
                StringSetIterator it = StringSetIteratorInit(tagset);
                while ((element = SetIteratorNext(&it)))
                {
                    if (!StringMatchFull(tag_regex, element))
                    {
                        pass = false;
                    }
                }
            }

            if (pass)
            {
                StringSetAdd(matching, expr);
            }
        }
        else
        {
            free(expr);
        }
    }

    return matching;
}

static FnCallResult FnCallVariablesMatching(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    if (!finalargs)
    {
        FatalError(ctx, "Function '%s' requires at least one argument", fp->name);
    }

    for (const Rlist *arg = finalargs; arg; arg = arg->next)
    {
        SyntaxTypeMatch err = CheckConstraintTypeMatch(fp->name, arg->val, DATA_TYPE_STRING, "", 1);
        if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
        {
            FatalError(ctx, "In function '%s', %s", fp->name, SyntaxTypeMatchToString(err));
        }
    }

    Rlist *matches = NULL;

    {
        VariableTableIterator *iter = EvalContextVariableTableIteratorNew(ctx, NULL, NULL, NULL);
        StringSet *global_matches = VariablesMatching(ctx, iter, finalargs);

        StringSetIterator it = StringSetIteratorInit(global_matches);
        const char *element = NULL;
        while ((element = StringSetIteratorNext(&it)))
        {
            RlistPrepend(&matches, element, RVAL_TYPE_SCALAR);
        }

        StringSetDestroy(global_matches);
        VariableTableIteratorDestroy(iter);
    }

    if (!matches)
    {
        RlistAppendScalarIdemp(&matches, CF_NULL_VALUE);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { matches, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallBundlesmatching(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buf[CF_BUFSIZE];
    char *regex = RlistScalarValue(finalargs);
    Rlist *matches = NULL;
    Policy *policy;

    if (!fp->caller)
    {
        FatalError(ctx, "Function '%s' had a null caller", fp->name);
    }

    policy = PolicyFromPromise(fp->caller);

    if (!policy)
    {
        FatalError(ctx, "Function '%s' had a null policy", fp->name);
    }

    if (!policy->bundles)
    {
        FatalError(ctx, "Function '%s' had null policy bundles", fp->name);
    }

    for (size_t i = 0; i < SeqLength(policy->bundles); i++)
    {
        const Bundle *bp = SeqAt(policy->bundles, i);
        if (!bp)
        {
            FatalError(ctx, "Function '%s' found null bundle at %ld", fp->name, i);
        }

        snprintf(buf, CF_BUFSIZE, "%s:%s", bp->ns, bp->name);
        if (StringMatchFull(regex, buf))
        {
            RlistPrepend(&matches, xstrdup(buf), RVAL_TYPE_SCALAR);
        }
    }

    if (!matches)
    {
        RlistAppendScalarIdemp(&matches, CF_NULL_VALUE);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { matches, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallCanonify(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buf[CF_BUFSIZE];
    char *string = RlistScalarValue(finalargs);

    buf[0] = '\0';
    
    if (!strcmp(fp->name, "canonifyuniquely"))
    {
        char hashbuffer[EVP_MAX_MD_SIZE * 4];
        unsigned char digest[EVP_MAX_MD_SIZE + 1];
        HashMethod type;

        type = HashMethodFromString("sha1");
        HashString(string, strlen(string), digest, type);
        snprintf(buf, CF_BUFSIZE, "%s_%s", string, SkipHashType(HashPrintSafe(type, digest, hashbuffer)));
    }
    else
    {
        snprintf(buf, CF_BUFSIZE, "%s", string);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(CanonifyName(buf)), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallTextXform(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buf[CF_BUFSIZE];
    char *string = RlistScalarValue(finalargs);
    int len = 0;

    memset(buf, 0, sizeof(buf));
    strncpy(buf, string, sizeof(buf) - 1);
    len = strlen(buf);

    if (!strcmp(fp->name, "downcase"))
    {
        int pos = 0;
        for (pos = 0; pos < len; pos++)
        {
            buf[pos] = tolower(buf[pos]);
        }
    }
    else if (!strcmp(fp->name, "upcase"))
    {
        int pos = 0;
        for (pos = 0; pos < len; pos++)
        {
            buf[pos] = toupper(buf[pos]);
        }
    }
    else if (!strcmp(fp->name, "reversestring"))
    {
        int c, i, j;
        for (i = 0, j = len - 1; i < j; i++, j--)
        {
            c = buf[i];
            buf[i] = buf[j];
            buf[j] = c;
        }
    }
    else if (!strcmp(fp->name, "strlen"))
    {
        sprintf(buf, "%d", len);
    }
    else if (!strcmp(fp->name, "head"))
    {
        long max = IntFromString(RlistScalarValue(finalargs->next));
        if (max < sizeof(buf))
        {
            buf[max] = '\0';
        }
    }
    else if (!strcmp(fp->name, "tail"))
    {
        long max = IntFromString(RlistScalarValue(finalargs->next));
        if (max < len)
        {
            strncpy(buf, string + len - max, sizeof(buf) - 1);
        }
    }
    else
    {
        Log(LOG_LEVEL_ERR, "text xform with unknown call function %s, aborting", fp->name);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buf), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallLastNode(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp, *newlist;

/* begin fn specific content */

    char *name = RlistScalarValue(finalargs);
    char *split = RlistScalarValue(finalargs->next);

    newlist = RlistFromSplitRegex(ctx, name, split, 100, true);

    for (rp = newlist; rp != NULL; rp = rp->next)
    {
        if (rp->next == NULL)
        {
            break;
        }
    }

    if (rp && rp->val.item)
    {
        char *res = xstrdup(RlistScalarValue(rp));

        RlistDestroy(newlist);
        return (FnCallResult) { FNCALL_SUCCESS, { res, RVAL_TYPE_SCALAR } };
    }
    else
    {
        RlistDestroy(newlist);
        return (FnCallResult) { FNCALL_FAILURE };
    }
}

/*******************************************************************/

static FnCallResult FnCallDirname(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char *dir = xstrdup(RlistScalarValue(finalargs));

    DeleteSlash(dir);
    ChopLastNode(dir);

    return (FnCallResult) { FNCALL_SUCCESS, { dir, RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallClassify(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    bool is_defined = IsDefinedClass(ctx, CanonifyName(RlistScalarValue(finalargs)), PromiseGetNamespace(fp->caller));

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(is_defined ? "any" : "!any"), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/
/* Executions                                                        */
/*********************************************************************/

static FnCallResult FnCallReturnsZero(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char comm[CF_BUFSIZE];
    const char *shellarg = RlistScalarValue(finalargs->next);
    ShellType shelltype;
    bool needExecutableCheck = false;
    if (strcmp(shellarg, "useshell") == 0)
    {
        shelltype = SHELL_TYPE_USE;
    }
    else if (strcmp(shellarg, "powershell") == 0)
    {
        shelltype = SHELL_TYPE_POWERSHELL;
    }
    else
    {
        shelltype = SHELL_TYPE_NONE;
    }

    if (IsAbsoluteFileName(RlistScalarValue(finalargs)))
    {
        needExecutableCheck = true;
    }
    else if (shelltype == SHELL_TYPE_NONE)
    {
        Log(LOG_LEVEL_ERR, "returnszero '%s' does not have an absolute path", RlistScalarValue(finalargs));
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
    }

    if (needExecutableCheck && !IsExecutable(CommandArg0(RlistScalarValue(finalargs))))
    {
        Log(LOG_LEVEL_ERR, "returnszero '%s' is assumed to be executable but isn't", RlistScalarValue(finalargs));
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
    }

    snprintf(comm, CF_BUFSIZE, "%s", RlistScalarValue(finalargs));

    if (ShellCommandReturnsZero(comm, shelltype))
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
    }
}

/*********************************************************************/

static FnCallResult FnCallExecResult(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
  /* execresult("/programpath",useshell|noshell|powershell) */
{
    const char *shellarg = RlistScalarValue(finalargs->next);
    ShellType shelltype;
    bool needExecutableCheck = false;
    if (strcmp(shellarg, "useshell") == 0)
    {
        shelltype = SHELL_TYPE_USE;
    }
    else if (strcmp(shellarg, "powershell") == 0)
    {
        shelltype = SHELL_TYPE_POWERSHELL;
    }
    else
    {
        shelltype = SHELL_TYPE_NONE;
    }

    if (IsAbsoluteFileName(RlistScalarValue(finalargs)))
    {
        needExecutableCheck = true;
    }
    else if (shelltype == SHELL_TYPE_NONE)
    {
        Log(LOG_LEVEL_ERR, "execresult '%s' does not have an absolute path", RlistScalarValue(finalargs));
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (needExecutableCheck && !IsExecutable(CommandArg0(RlistScalarValue(finalargs))))
    {
        Log(LOG_LEVEL_ERR, "execresult '%s' is assumed to be executable but isn't", RlistScalarValue(finalargs));
        return (FnCallResult) { FNCALL_FAILURE };
    }

    char buffer[CF_EXPANDSIZE];

    if (GetExecOutput(RlistScalarValue(finalargs), buffer, shelltype))
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
}

/*********************************************************************/

static FnCallResult FnCallUseModule(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
  /* usemodule("/programpath",varargs) */
{
    char modulecmd[CF_BUFSIZE];
    struct stat statbuf;

/* begin fn specific content */

    char *command = RlistScalarValue(finalargs);
    char *args = RlistScalarValue(finalargs->next);

    snprintf(modulecmd, CF_BUFSIZE, "\"%s%cmodules%c%s\"", CFWORKDIR, FILE_SEPARATOR, FILE_SEPARATOR, command);

    if (stat(CommandArg0(modulecmd), &statbuf) == -1)
    {
        Log(LOG_LEVEL_ERR, "Plug-in module '%s' not found", modulecmd);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if ((statbuf.st_uid != 0) && (statbuf.st_uid != getuid()))
    {
        Log(LOG_LEVEL_ERR, "Module '%s' was not owned by uid %ju who is executing agent", modulecmd, (uintmax_t)getuid());
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (!JoinPath(modulecmd, args))
    {
        Log(LOG_LEVEL_ERR, "Culprit: class list for module (shouldn't happen)");
        return (FnCallResult) { FNCALL_FAILURE };
    }

    snprintf(modulecmd, CF_BUFSIZE, "\"%s%cmodules%c%s\" %s", CFWORKDIR, FILE_SEPARATOR, FILE_SEPARATOR, command, args);
    Log(LOG_LEVEL_VERBOSE, "Executing and using module [%s]", modulecmd);

    if (!ExecModule(ctx, modulecmd, PromiseGetNamespace(fp->caller)))
    {
        return (FnCallResult) { FNCALL_FAILURE};
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/
/* Misc                                                              */
/*********************************************************************/

static FnCallResult FnCallSplayClass(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE], class[CF_MAXVARSIZE];

    Interval policy = IntervalFromString(RlistScalarValue(finalargs->next));

    if (policy == INTERVAL_HOURLY)
    {
        /* 12 5-minute slots in hour */
        int slot = StringHash(RlistScalarValue(finalargs), 0, CF_HASHTABLESIZE) * 12 / CF_HASHTABLESIZE;
        snprintf(class, CF_MAXVARSIZE, "Min%02d_%02d", slot * 5, ((slot + 1) * 5) % 60);
    }
    else
    {
        /* 12*24 5-minute slots in day */
        int dayslot = StringHash(RlistScalarValue(finalargs), 0, CF_HASHTABLESIZE) * 12 * 24 / CF_HASHTABLESIZE;
        int hour = dayslot / 12;
        int slot = dayslot % 12;

        snprintf(class, CF_MAXVARSIZE, "Min%02d_%02d.Hr%02d", slot * 5, ((slot + 1) * 5) % 60, hour);
    }

    if (IsDefinedClass(ctx, class, PromiseGetNamespace(fp->caller)))
    {
        strcpy(buffer, "any");
    }
    else
    {
        strcpy(buffer, "!any");
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallReadTcp(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
 /* ReadTCP(localhost,80,'GET index.html',1000) */
{
    AgentConnection *conn = NULL;
    char buffer[CF_BUFSIZE];
    int val = 0, n_read = 0;
    short portnum;

    memset(buffer, 0, sizeof(buffer));

/* begin fn specific content */

    char *hostnameip = RlistScalarValue(finalargs);
    char *port = RlistScalarValue(finalargs->next);
    char *sendstring = RlistScalarValue(finalargs->next->next);
    char *maxbytes = RlistScalarValue(finalargs->next->next->next);

    val = IntFromString(maxbytes);
    portnum = (short) IntFromString(port);

    if (val < 0 || portnum < 0 || THIS_AGENT_TYPE == AGENT_TYPE_COMMON)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (val > CF_BUFSIZE - 1)
    {
        Log(LOG_LEVEL_ERR, "Too many bytes to read from TCP port '%s@%s'", port, hostnameip);
        val = CF_BUFSIZE - CF_BUFFERMARGIN;
    }

    Log(LOG_LEVEL_DEBUG, "Want to read %d bytes from port %d at '%s'", val, portnum, hostnameip);

    conn = NewAgentConn(hostnameip);

    FileCopy fc = {
        .force_ipv4 = false,
        .portnumber = portnum,
    };

    /* TODO don't use ServerConnect, this is only for CFEngine connections! */

    if (!ServerConnect(conn, hostnameip, fc))
    {
        Log(LOG_LEVEL_INFO, "Couldn't open a tcp socket. (socket: %s)", GetErrorStr());
        DeleteAgentConn(conn);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (strlen(sendstring) > 0)
    {
        int sent = 0;
        int result = 0;
        size_t length = strlen(sendstring);
        do {
            result = send(conn->conn_info.sd, sendstring, length, 0);
            if (result < 0)
            {
                cf_closesocket(conn->conn_info.sd);
                DeleteAgentConn(conn);
                return (FnCallResult) { FNCALL_FAILURE };
            }
            else
            {
                sent += result;
            }
        } while (sent < length);
    }

    if ((n_read = recv(conn->conn_info.sd, buffer, val, 0)) == -1)
    {
    }

    if (n_read == -1)
    {
        cf_closesocket(conn->conn_info.sd);
        DeleteAgentConn(conn);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    cf_closesocket(conn->conn_info.sd);
    DeleteAgentConn(conn);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallRegList(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    const char *listvar = RlistScalarValue(finalargs);
    const char *regex = RlistScalarValue(finalargs->next);

    if (!IsVarList(listvar))
    {
        Log(LOG_LEVEL_VERBOSE, "Function reglist was promised a list called '%s' but this was not found", listvar);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    char naked[CF_MAXVARSIZE] = "";
    GetNaked(naked, listvar);

    VarRef *ref = VarRefParse(naked);

    Rval retval;
    if (!EvalContextVariableGet(ctx, ref, &retval, NULL))
    {
        Log(LOG_LEVEL_VERBOSE, "Function REGLIST was promised a list called '%s' but this was not found", listvar);
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (retval.type != RVAL_TYPE_LIST)
    {
        Log(LOG_LEVEL_VERBOSE, "Function reglist was promised a list called '%s' but this variable is not a list",
              listvar);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    const Rlist *list = retval.item;

    char buffer[CF_BUFSIZE];
    strcpy(buffer, "!any");

    for (const Rlist *rp = list; rp != NULL; rp = rp->next)
    {
        if (strcmp(RlistScalarValue(rp), CF_NULL_VALUE) == 0)
        {
            continue;
        }

        if (FullTextMatch(ctx, regex, RlistScalarValue(rp)))
        {
            strcpy(buffer, "any");
            break;
        }
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallRegArray(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char *arrayname = RlistScalarValue(finalargs);
    char *regex = RlistScalarValue(finalargs->next);

    VarRef *ref = VarRefParse(arrayname);
    bool found = false;

    VariableTableIterator *iter = EvalContextVariableTableIteratorNew(ctx, ref->ns, ref->scope, ref->lval);
    Variable *var = NULL;
    while ((var = VariableTableIteratorNext(iter)))
    {
        if (FullTextMatch(ctx, regex, RvalScalarValue(var->rval)))
        {
            found = true;
            break;
        }
    }
    VariableTableIteratorDestroy(iter);

    if (found)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
    }
}


static FnCallResult FnCallGetIndices(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    VarRef *ref = VarRefParseFromBundle(RlistScalarValue(finalargs), PromiseGetBundle(fp->caller));

    DataType type = DATA_TYPE_NONE;
    Rval rval;
    EvalContextVariableGet(ctx, ref, &rval, &type);

    Rlist *keys = NULL;
    if (type == DATA_TYPE_CONTAINER)
    {
        if (JsonGetElementType(RvalContainerValue(rval)) == JSON_ELEMENT_TYPE_CONTAINER)
        {
            if (JsonGetContrainerType(RvalContainerValue(rval)) == JSON_CONTAINER_TYPE_OBJECT)
            {
                JsonIterator iter = JsonIteratorInit(RvalContainerValue(rval));
                const char *key = NULL;
                while ((key = JsonIteratorNextKey(&iter)))
                {
                    RlistAppendScalar(&keys, key);
                }
            }
            else
            {
                for (size_t i = 0; i < JsonLength(RvalContainerValue(rval)); i++)
                {
                    Rval key = (Rval) { StringFromLong(i), RVAL_TYPE_SCALAR };
                    RlistAppendRval(&keys, key);
                }
            }
        }
    }
    else
    {
        VariableTableIterator *iter = EvalContextVariableTableIteratorNew(ctx, ref->ns, ref->scope, ref->lval);
        Variable *var = NULL;
        while ((var = VariableTableIteratorNext(iter)))
        {
            for (size_t i = 0; i < var->ref->num_indices; i++)
            {
                RlistAppendScalarIdemp(&keys, var->ref->indices[i]);
            }
        }
        VariableTableIteratorDestroy(iter);
    }

    VarRefDestroy(ref);

    if (RlistLen(keys) == 0)
    {
        RlistAppendScalarIdemp(&keys, CF_NULL_VALUE);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { keys, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallGetValues(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    VarRef *ref = VarRefParseFromBundle(RlistScalarValue(finalargs), PromiseGetBundle(fp->caller));

    DataType type = DATA_TYPE_NONE;
    Rval rval;
    EvalContextVariableGet(ctx, ref, &rval, &type);

    Rlist *values = NULL;
    if (type == DATA_TYPE_CONTAINER)
    {
        if (JsonGetElementType(RvalContainerValue(rval)) == JSON_ELEMENT_TYPE_CONTAINER)
        {
            JsonIterator iter = JsonIteratorInit(RvalContainerValue(rval));
            const JsonElement *el = NULL;
            while ((el = JsonIteratorNextValue(&iter)))
            {
                if (JsonGetElementType(el) != JSON_ELEMENT_TYPE_PRIMITIVE)
                {
                    continue;
                }

                switch (JsonGetPrimitiveType(el))
                {
                case JSON_PRIMITIVE_TYPE_BOOL:
                    RlistAppendScalar(&values, JsonPrimitiveGetAsBool(el) ? "true" : "false");
                    break;
                case JSON_PRIMITIVE_TYPE_INTEGER:
                    {
                        char *str = StringFromLong(JsonPrimitiveGetAsInteger(el));
                        RlistAppendScalar(&values, str);
                        free(str);
                    }
                    break;
                case JSON_PRIMITIVE_TYPE_REAL:
                    {
                        char *str = StringFromDouble(JsonPrimitiveGetAsReal(el));
                        RlistAppendScalar(&values, str);
                        free(str);
                    }
                    break;
                case JSON_PRIMITIVE_TYPE_STRING:
                    RlistAppendScalar(&values, JsonPrimitiveGetAsString(el));
                    break;

                case JSON_PRIMITIVE_TYPE_NULL:
                    break;
                }
            }
        }
    }
    else
    {
        VariableTableIterator *iter = EvalContextVariableTableIteratorNew(ctx, ref->ns, ref->scope, ref->lval);
        Variable *var = NULL;
        while ((var = VariableTableIteratorNext(iter)))
        {
            if (var->ref->num_indices != 1)
            {
                continue;
            }

            switch (var->rval.type)
            {
            case RVAL_TYPE_SCALAR:
                RlistAppendScalarIdemp(&values, var->rval.item);
                break;

            case RVAL_TYPE_LIST:
                for (const Rlist *rp = var->rval.item; rp != NULL; rp = rp->next)
                {
                    RlistAppendScalarIdemp(&values, RlistScalarValue(rp));
                }
                break;

            default:
                break;
            }
        }
    }

    VarRefDestroy(ref);

    if (RlistLen(values) == 0)
    {
        RlistAppendScalarIdemp(&values, CF_NULL_VALUE);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { values, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallGrep(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    return FilterInternal(ctx,
                          fp,
                          RlistScalarValue(finalargs), // regex
                          RlistScalarValue(finalargs->next), // list identifier
                          1, // regex match = TRUE
                          0, // invert matches = FALSE
                          99999999999); // max results = max int
}

/*********************************************************************/

static FnCallResult FnCallSum(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_MAXVARSIZE];
    Rval rval2;
    double sum = 0;

    VarRef *ref = VarRefParse(RlistScalarValue(finalargs));

    if (!EvalContextVariableGet(ctx, ref, &rval2, NULL))
    {
        Log(LOG_LEVEL_VERBOSE, "Function sum was promised a list called '%s' but this was not found", ref->lval);
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (rval2.type != RVAL_TYPE_LIST)
    {
        Log(LOG_LEVEL_VERBOSE, "Function sum was promised a list called '%s' but this was not found", ref->lval);
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    VarRefDestroy(ref);

    for (const Rlist *rp = (const Rlist *) rval2.item; rp != NULL; rp = rp->next)
    {
        double x;

        if (!DoubleFromString(RlistScalarValue(rp), &x))
        {
            return (FnCallResult) { FNCALL_FAILURE };
        }
        else
        {
            sum += x;
        }
    }

    snprintf(buffer, CF_MAXVARSIZE, "%lf", sum);
    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallProduct(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_MAXVARSIZE];
    Rval rval2;
    double product = 1.0;

    VarRef *ref = VarRefParse(RlistScalarValue(finalargs));

    if (!EvalContextVariableGet(ctx, ref, &rval2, NULL))
    {
        Log(LOG_LEVEL_VERBOSE, "Function 'product' was promised a list called '%s' but this was not found", ref->lval);
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (rval2.type != RVAL_TYPE_LIST)
    {
        Log(LOG_LEVEL_VERBOSE, "Function 'product' was promised a list called '%s' but this was not found", ref->lval);
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    VarRefDestroy(ref);

    for (const Rlist *rp = (const Rlist *) rval2.item; rp != NULL; rp = rp->next)
    {
        double x;
        if (!DoubleFromString(RlistScalarValue(rp), &x))
        {
            return (FnCallResult) { FNCALL_FAILURE };
        }
        else
        {
            product *= x;
        }
    }

    snprintf(buffer, CF_MAXVARSIZE, "%lf", product);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallJoin(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char *joined = NULL;
    Rval rval2;
    int size = 0;

    const char *join = RlistScalarValue(finalargs);
    VarRef *ref = VarRefParse(RlistScalarValue(finalargs->next));

    if (!EvalContextVariableGet(ctx, ref, &rval2, NULL))
    {
        Log(LOG_LEVEL_VERBOSE, "Function 'join' was promised a list called '%s.%s' but this was not (yet) found", ref->scope, ref->lval);
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (rval2.type != RVAL_TYPE_LIST)
    {
        Log(LOG_LEVEL_VERBOSE, "Function 'join' was promised a list called '%s' but this was not (yet) found", ref->lval);
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    VarRefDestroy(ref);

    for (const Rlist *rp = RvalRlistValue(rval2); rp != NULL; rp = rp->next)
    {
        if (strcmp(RlistScalarValue(rp), CF_NULL_VALUE) == 0)
        {
            continue;
        }

        size += strlen(RlistScalarValue(rp)) + strlen(join);
    }

    joined = xcalloc(1, size + 1);
    size = 0;

    for (const Rlist *rp = (const Rlist *) rval2.item; rp != NULL; rp = rp->next)
    {
        if (strcmp(RlistScalarValue(rp), CF_NULL_VALUE) == 0)
        {
            continue;
        }

        strcpy(joined + size, RlistScalarValue(rp));

        if (rp->next != NULL)
        {
            strcpy(joined + size + strlen(RlistScalarValue(rp)), join);
            size += strlen(RlistScalarValue(rp)) + strlen(join);
        }
    }

    return (FnCallResult) { FNCALL_SUCCESS, { joined, RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallGetFields(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp, *newlist;
    char name[CF_MAXVARSIZE], retval[CF_SMALLBUF];
    int lcount = 0, vcount = 0, nopurge = true;
    FILE *fin;

/* begin fn specific content */

    char *regex = RlistScalarValue(finalargs);
    char *filename = RlistScalarValue(finalargs->next);
    char *split = RlistScalarValue(finalargs->next->next);
    char *array_lval = RlistScalarValue(finalargs->next->next->next);

    if ((fin = fopen(filename, "r")) == NULL)
    {
        Log(LOG_LEVEL_ERR, "File '%s' could not be read in getfields(). (fopen: %s)", filename, GetErrorStr());
        return (FnCallResult) { FNCALL_FAILURE };
    }

    for (;;)
    {
        char line[CF_BUFSIZE];

        if (fgets(line, sizeof(line), fin) == NULL)
        {
            if (ferror(fin))
            {
                Log(LOG_LEVEL_ERR, "Unable to read data from file '%s'. (fgets: %s)", filename, GetErrorStr());
                fclose(fin);
                return (FnCallResult) { FNCALL_FAILURE };
            }
            else /* feof */
            {
                break;
            }
        }

        if (Chop(line, CF_EXPANDSIZE) == -1)
        {
            Log(LOG_LEVEL_ERR, "Chop was called on a string that seemed to have no terminator");
        }

        if (!FullTextMatch(ctx, regex, line))
        {
            continue;
        }

        if (lcount == 0)
        {
            newlist = RlistFromSplitRegex(ctx, line, split, 31, nopurge);

            vcount = 1;

            for (rp = newlist; rp != NULL; rp = rp->next)
            {
                snprintf(name, CF_MAXVARSIZE - 1, "%s[%d]", array_lval, vcount);
                VarRef *ref = VarRefParseFromBundle(name, PromiseGetBundle(fp->caller));
                EvalContextVariablePut(ctx, ref, RlistScalarValue(rp), DATA_TYPE_STRING);
                VarRefDestroy(ref);
                Log(LOG_LEVEL_VERBOSE, "getfields: defining '%s' => '%s'", name, RlistScalarValue(rp));
                vcount++;
            }
        }

        lcount++;
    }

    fclose(fin);

    snprintf(retval, CF_SMALLBUF - 1, "%d", lcount);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(retval), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallCountLinesMatching(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char retval[CF_SMALLBUF];
    int lcount = 0;
    FILE *fin;

/* begin fn specific content */

    char *regex = RlistScalarValue(finalargs);
    char *filename = RlistScalarValue(finalargs->next);

    if ((fin = fopen(filename, "r")) == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "File '%s' could not be read in countlinesmatching(). (fopen: %s)", filename, GetErrorStr());
        snprintf(retval, CF_SMALLBUF - 1, "0");
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(retval), RVAL_TYPE_SCALAR } };
    }

    for (;;)
    {
        char line[CF_BUFSIZE];
        if (fgets(line, sizeof(line), fin) == NULL)
        {
            if (ferror(fin))
            {
                Log(LOG_LEVEL_ERR, "Unable to read data from file '%s'. (fgets: %s)", filename, GetErrorStr());
                fclose(fin);
                return (FnCallResult) { FNCALL_FAILURE };
            }
            else /* feof */
            {
                break;
            }
        }
        if (Chop(line, CF_EXPANDSIZE) == -1)
        {
            Log(LOG_LEVEL_ERR, "Chop was called on a string that seemed to have no terminator");
        }

        if (FullTextMatch(ctx, regex, line))
        {
            lcount++;
            Log(LOG_LEVEL_VERBOSE, "countlinesmatching: matched '%s'", line);
            continue;
        }
    }

    fclose(fin);

    snprintf(retval, CF_SMALLBUF - 1, "%d", lcount);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(retval), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallLsDir(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char line[CF_BUFSIZE];
    Dir *dirh = NULL;
    const struct dirent *dirp;
    Rlist *newlist = NULL;

/* begin fn specific content */

    char *dirname = RlistScalarValue(finalargs);
    char *regex = RlistScalarValue(finalargs->next);
    int includepath = BooleanFromString(RlistScalarValue(finalargs->next->next));

    dirh = DirOpen(dirname);

    if (dirh == NULL)
    {
        Log(LOG_LEVEL_ERR, "Directory '%s' could not be accessed in lsdir(), (opendir: %s)", dirname, GetErrorStr());
        RlistPrepend(&newlist, CF_NULL_VALUE, RVAL_TYPE_SCALAR);
        return (FnCallResult) { FNCALL_SUCCESS, { newlist, RVAL_TYPE_LIST } };
    }

    for (dirp = DirRead(dirh); dirp != NULL; dirp = DirRead(dirh))
    {
        if (strlen(regex) == 0 || FullTextMatch(ctx, regex, dirp->d_name))
        {
            if (includepath)
            {
                snprintf(line, CF_BUFSIZE, "%s/%s", dirname, dirp->d_name);
                MapName(line);
                RlistPrepend(&newlist, line, RVAL_TYPE_SCALAR);
            }
            else
            {
                RlistPrepend(&newlist, dirp->d_name, RVAL_TYPE_SCALAR);
            }
        }
    }

    DirClose(dirh);

    if (newlist == NULL)
    {
        RlistPrepend(&newlist, CF_NULL_VALUE, RVAL_TYPE_SCALAR);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { newlist, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallMapArray(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char expbuf[CF_EXPANDSIZE];
    Rlist *returnlist = NULL;

    char *map = RlistScalarValue(finalargs);

    VarRef *ref = VarRefParseFromBundle(RlistScalarValue(finalargs->next), PromiseGetBundle(fp->caller));

    VariableTableIterator *iter = EvalContextVariableTableIteratorNew(ctx, ref->ns, ref->scope, ref->lval);
    Variable *var = NULL;

    while ((var = VariableTableIteratorNext(iter)))
    {
        if (var->ref->num_indices != 1)
        {
            continue;
        }

        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_THIS, "k", var->ref->indices[0], DATA_TYPE_STRING);

        switch (var->rval.type)
        {
        case RVAL_TYPE_SCALAR:
            EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_THIS, "v", var->rval.item, DATA_TYPE_STRING);
            ExpandScalar(ctx, PromiseGetBundle(fp->caller)->ns, PromiseGetBundle(fp->caller)->name, map, expbuf);

            if (strstr(expbuf, "$(this.k)") || strstr(expbuf, "${this.k}") ||
                strstr(expbuf, "$(this.v)") || strstr(expbuf, "${this.v}"))
            {
                RlistDestroy(returnlist);
                EvalContextVariableRemoveSpecial(ctx, SPECIAL_SCOPE_THIS, "k");
                EvalContextVariableRemoveSpecial(ctx, SPECIAL_SCOPE_THIS, "v");
                return (FnCallResult) { FNCALL_FAILURE };
            }

            RlistAppendScalar(&returnlist, expbuf);
            EvalContextVariableRemoveSpecial(ctx, SPECIAL_SCOPE_THIS, "v");
            break;

        case RVAL_TYPE_LIST:
            for (const Rlist *rp = var->rval.item; rp != NULL; rp = rp->next)
            {
                EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_THIS, "v", RlistScalarValue(rp), DATA_TYPE_STRING);
                ExpandScalar(ctx, PromiseGetBundle(fp->caller)->ns, PromiseGetBundle(fp->caller)->name, map, expbuf);

                if (strstr(expbuf, "$(this.k)") || strstr(expbuf, "${this.k}") ||
                    strstr(expbuf, "$(this.v)") || strstr(expbuf, "${this.v}"))
                {
                    RlistDestroy(returnlist);
                    EvalContextVariableRemoveSpecial(ctx, SPECIAL_SCOPE_THIS, "k");
                    EvalContextVariableRemoveSpecial(ctx, SPECIAL_SCOPE_THIS, "v");
                    return (FnCallResult) { FNCALL_FAILURE };
                }

                RlistAppendScalarIdemp(&returnlist, expbuf);
                EvalContextVariableRemoveSpecial(ctx, SPECIAL_SCOPE_THIS, "v");
            }
            break;

        default:
            break;
        }
        EvalContextVariableRemoveSpecial(ctx, SPECIAL_SCOPE_THIS, "k");
    }

    VariableTableIteratorDestroy(iter);
    VarRefDestroy(ref);

    if (returnlist == NULL)
    {
        RlistAppendScalarIdemp(&returnlist, CF_NULL_VALUE);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { returnlist, RVAL_TYPE_LIST } };
}

static FnCallResult FnCallMapList(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char expbuf[CF_EXPANDSIZE];
    Rlist *newlist = NULL;
    Rval rval;
    DataType retype;

    const char *map = RlistScalarValue(finalargs);
    char *listvar = RlistScalarValue(finalargs->next);

    char naked[CF_MAXVARSIZE] = "";
    if (IsVarList(listvar))
    {
        GetNaked(naked, listvar);
    }
    else
    {
        strncpy(naked, listvar, CF_MAXVARSIZE - 1);
    }

    VarRef *ref = VarRefParse(naked);

    retype = DATA_TYPE_NONE;
    if (!EvalContextVariableGet(ctx, ref, &rval, &retype))
    {
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    VarRefDestroy(ref);

    if (retype != DATA_TYPE_STRING_LIST && retype != DATA_TYPE_INT_LIST && retype != DATA_TYPE_REAL_LIST)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    for (const Rlist *rp = RvalRlistValue(rval); rp != NULL; rp = rp->next)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_THIS, "this", RlistScalarValue(rp), DATA_TYPE_STRING);

        ExpandScalar(ctx, NULL, "this", map, expbuf);

        if (strstr(expbuf, "$(this)") || strstr(expbuf, "${this}"))
        {
            RlistDestroy(newlist);
            EvalContextVariableRemoveSpecial(ctx, SPECIAL_SCOPE_THIS, "this");
            return (FnCallResult) { FNCALL_FAILURE };
        }

        RlistAppendScalar(&newlist, expbuf);
        EvalContextVariableRemoveSpecial(ctx, SPECIAL_SCOPE_THIS, "this");
    }

    return (FnCallResult) { FNCALL_SUCCESS, { newlist, RVAL_TYPE_LIST } };
}

static FnCallResult FnCallMergeData(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    if (RlistLen(args) == 0)
    {
        Log(LOG_LEVEL_ERR, "Function mergedata needs at least one argument, a reference to a container variable");
        return (FnCallResult)  { FNCALL_FAILURE };
    }

    for (const Rlist *arg = args; arg; arg = arg->next)
    {
        if (args->val.type != RVAL_TYPE_SCALAR)
        {
            Log(LOG_LEVEL_ERR, "Function mergedata, argument '%s' is not a variable reference", RlistScalarValue(arg));
            return (FnCallResult)  { FNCALL_FAILURE };
        }
    }

    Seq *containers = SeqNew(10, NULL);
    for (const Rlist *arg = args; arg; arg = arg->next)
    {
        VarRef *ref = VarRefParseFromBundle(RlistScalarValue(arg), PromiseGetBundle(fp->caller));

        Rval rval;
        if (!EvalContextVariableGet(ctx, ref, &rval, NULL))
        {
            Log(LOG_LEVEL_ERR, "Function mergedata, argument '%s' does not resolve to a container", RlistScalarValue(arg));
            SeqDestroy(containers);
            VarRefDestroy(ref);
            return (FnCallResult)  { FNCALL_FAILURE };
        }

        SeqAppend(containers, RvalContainerValue(rval));

        VarRefDestroy(ref);
    }

    if (SeqLength(containers) == 1)
    {
        JsonElement *first = SeqAt(containers, 0);
        SeqDestroy(containers);
        return  (FnCallResult) { FNCALL_SUCCESS, (Rval) { JsonCopy(first), RVAL_TYPE_CONTAINER } };
    }
    else
    {
        JsonElement *first = SeqAt(containers, 0);
        JsonElement *second = SeqAt(containers, 1);
        JsonElement *result = JsonMerge(first, second);

        for (size_t i = 2; i < SeqLength(containers); i++)
        {
            JsonElement *cur = SeqAt(containers, i);
            JsonElement *tmp = JsonMerge(result, cur);
            JsonDestroy(result);
            result = tmp;
        }

        SeqDestroy(containers);
        return (FnCallResult) { FNCALL_SUCCESS, (Rval) { result, RVAL_TYPE_CONTAINER } };
    }

    assert(false);
}


static FnCallResult FnCallSelectServers(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
 /* ReadTCP(localhost,80,'GET index.html',1000) */
{
    AgentConnection *conn = NULL;
    Rlist *hostnameip;
    char buffer[CF_BUFSIZE], naked[CF_MAXVARSIZE];
    int val = 0, n_read = 0, count = 0;
    short portnum;
    Rval retval;
    buffer[0] = '\0';

    char *listvar = RlistScalarValue(finalargs);
    char *port = RlistScalarValue(finalargs->next);
    char *sendstring = RlistScalarValue(finalargs->next->next);
    char *regex = RlistScalarValue(finalargs->next->next->next);
    char *maxbytes = RlistScalarValue(finalargs->next->next->next->next);
    char *array_lval = RlistScalarValue(finalargs->next->next->next->next->next);

    if (IsVarList(listvar))
    {
        GetNaked(naked, listvar);
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Function selectservers was promised a list called '%s' but this was not found", listvar);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    VarRef *ref = VarRefParse(naked);

    if (!EvalContextVariableGet(ctx, ref, &retval, NULL))
    {
        Log(LOG_LEVEL_VERBOSE,
            "Function selectservers was promised a list called '%s' but this was not found from context '%s.%s'",
              listvar, ref->scope, naked);
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    VarRefDestroy(ref);

    if (retval.type != RVAL_TYPE_LIST)
    {
        Log(LOG_LEVEL_VERBOSE,
            "Function selectservers was promised a list called '%s' but this variable is not a list", listvar);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    hostnameip = RvalRlistValue(retval);
    val = IntFromString(maxbytes);
    portnum = (short) IntFromString(port);

    if (val < 0 || portnum < 0)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (val > CF_BUFSIZE - 1)
    {
        Log(LOG_LEVEL_ERR, "Too many bytes specificed in selectservers");
        val = CF_BUFSIZE - CF_BUFFERMARGIN;
    }

    if (THIS_AGENT_TYPE != AGENT_TYPE_AGENT)
    {
        snprintf(buffer, CF_MAXVARSIZE - 1, "%d", count);
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
    }

    Policy *select_server_policy = PolicyNew();
    Promise *pp = NULL;
    {
        Bundle *bp = PolicyAppendBundle(select_server_policy, NamespaceDefault(), "select_server_bundle", "agent", NULL, NULL);
        PromiseType *tp = BundleAppendPromiseType(bp, "select_server");

        pp = PromiseTypeAppendPromise(tp, "function", (Rval) { NULL, RVAL_TYPE_NOPROMISEE }, NULL);
    }

    assert(pp);

    for (Rlist *rp = hostnameip; rp != NULL; rp = rp->next)
    {
        Log(LOG_LEVEL_DEBUG, "Want to read %d bytes from port %d at '%s'", val, portnum, RlistScalarValue(rp));

        conn = NewAgentConn(RlistScalarValue(rp));

        FileCopy fc = {
            .force_ipv4 = false,
            .portnumber = portnum,
        };

        /* TODO don't use ServerConnect, this is only for CFEngine connections! */

        if (!ServerConnect(conn, RlistScalarValue(rp), fc))
        {
            Log(LOG_LEVEL_INFO, "Couldn't open a tcp socket. (socket %s)", GetErrorStr());
            DeleteAgentConn(conn);
            continue;
        }

        if (strlen(sendstring) > 0)
        {
            if (SendSocketStream(conn->conn_info.sd, sendstring, strlen(sendstring)) == -1)
            {
                cf_closesocket(conn->conn_info.sd);
                DeleteAgentConn(conn);
                continue;
            }

            if ((n_read = recv(conn->conn_info.sd, buffer, val, 0)) == -1)
            {
            }

            if (n_read == -1)
            {
                cf_closesocket(conn->conn_info.sd);
                DeleteAgentConn(conn);
                continue;
            }

            if (strlen(regex) == 0 || FullTextMatch(ctx, regex, buffer))
            {
                Log(LOG_LEVEL_VERBOSE, "Host '%s' is alive and responding correctly", RlistScalarValue(rp));
                snprintf(buffer, CF_MAXVARSIZE - 1, "%s[%d]", array_lval, count);
                VarRef *ref = VarRefParseFromBundle(buffer, PromiseGetBundle(fp->caller));
                EvalContextVariablePut(ctx, ref, RvalScalarValue(rp->val), DATA_TYPE_STRING);
                VarRefDestroy(ref);
                count++;
            }
        }
        else
        {
            Log(LOG_LEVEL_VERBOSE, "Host '%s' is alive", RlistScalarValue(rp));
            snprintf(buffer, CF_MAXVARSIZE - 1, "%s[%d]", array_lval, count);
            VarRef *ref = VarRefParseFromBundle(buffer, PromiseGetBundle(fp->caller));
            EvalContextVariablePut(ctx, ref, RvalScalarValue(rp->val), DATA_TYPE_STRING);
            VarRefDestroy(ref);

            if (IsDefinedClass(ctx, CanonifyName(RlistScalarValue(rp)), PromiseGetNamespace(fp->caller)))
            {
                Log(LOG_LEVEL_VERBOSE, "This host is in the list and has promised to join the class '%s' - joined",
                      array_lval);
                EvalContextClassPut(ctx, PromiseGetNamespace(fp->caller), array_lval, true, CONTEXT_SCOPE_NAMESPACE);
            }

            count++;
        }

        cf_closesocket(conn->conn_info.sd);
        DeleteAgentConn(conn);
    }

    PolicyDestroy(select_server_policy);

/* Return the subset that is alive and responding correctly */

/* Return the number of lines in array */

    snprintf(buffer, CF_MAXVARSIZE - 1, "%d", count);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}


static FnCallResult FnCallShuffle(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    const char *seed_str = RlistScalarValue(finalargs->next);

    Rval list_rval;
    DataType list_dtype = DATA_TYPE_NONE;

    if (!GetListReferenceArgument(ctx, fp, RlistScalarValue(finalargs), &list_rval, &list_dtype))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (list_dtype != DATA_TYPE_STRING_LIST)
    {
        Log(LOG_LEVEL_ERR, "Function '%s' expected a variable that resolves to a string list, got '%s'", fp->name, DataTypeToString(list_dtype));
        return (FnCallResult) { FNCALL_FAILURE };
    }

    Seq *seq = SeqNew(1000, NULL);
    for (const Rlist *rp = list_rval.item; rp; rp = rp->next)
    {
        SeqAppend(seq, RlistScalarValue(rp));
    }

    SeqShuffle(seq, StringHash(seed_str, 0, RAND_MAX));

    Rlist *shuffled = NULL;
    for (size_t i = 0; i < SeqLength(seq); i++)
    {
        RlistPrepend(&shuffled, SeqAt(seq, i), RVAL_TYPE_SCALAR);
    }

    SeqDestroy(seq);
    return (FnCallResult) { FNCALL_SUCCESS, (Rval) { shuffled, RVAL_TYPE_LIST } };
}


static FnCallResult FnCallIsNewerThan(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    struct stat frombuf, tobuf;

/* begin fn specific content */

    if (stat(RlistScalarValue(finalargs), &frombuf) == -1)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (stat(RlistScalarValue(finalargs->next), &tobuf) == -1)
    {
        return (FnCallResult) { FNCALL_FAILURE};
    }

    if (frombuf.st_mtime > tobuf.st_mtime)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
    }
}

/*********************************************************************/

static FnCallResult FnCallIsAccessedBefore(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    struct stat frombuf, tobuf;

/* begin fn specific content */

    if (stat(RlistScalarValue(finalargs), &frombuf) == -1)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (stat(RlistScalarValue(finalargs->next), &tobuf) == -1)
    {
        return (FnCallResult) { FNCALL_FAILURE};
    }

    if (frombuf.st_atime < tobuf.st_atime)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
    }
}

/*********************************************************************/

static FnCallResult FnCallIsChangedBefore(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    struct stat frombuf, tobuf;

/* begin fn specific content */

    if (stat(RlistScalarValue(finalargs), &frombuf) == -1)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
    else if (stat(RlistScalarValue(finalargs->next), &tobuf) == -1)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (frombuf.st_ctime > tobuf.st_ctime)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
    }
}

/*********************************************************************/

static FnCallResult FnCallFileStat(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE], *path = RlistScalarValue(finalargs);
    struct stat statbuf;

    buffer[0] = '\0';

/* begin fn specific content */

    if (lstat(path, &statbuf) == -1)
    {
        if (!strcmp(fp->name, "filesize"))
        {
            return (FnCallResult) { FNCALL_FAILURE };
        }

        strcpy(buffer, "!any");
    }
    else
    {
        strcpy(buffer, "!any");

        if (!strcmp(fp->name, "isexecutable"))
        {
            if (IsExecutable(path))
            {
                strcpy(buffer, "any");
            }
        }
        else if (!strcmp(fp->name, "isdir"))
        {
            if (S_ISDIR(statbuf.st_mode))
            {
                strcpy(buffer, "any");
            }
        }
        else if (!strcmp(fp->name, "islink"))
        {
            if (S_ISLNK(statbuf.st_mode))
            {
                strcpy(buffer, "any");
            }
        }
        else if (!strcmp(fp->name, "isplain"))
        {
            if (S_ISREG(statbuf.st_mode))
            {
                strcpy(buffer, "any");
            }
        }
        else if (!strcmp(fp->name, "fileexists"))
        {
            strcpy(buffer, "any");
        }
        else if (!strcmp(fp->name, "filesize"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_size);
        }
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallFileStatDetails(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE], *path = RlistScalarValue(finalargs);
    char *detail = RlistScalarValue(finalargs->next);
    struct stat statbuf;

    buffer[0] = '\0';

/* begin fn specific content */

    if (lstat(path, &statbuf) == -1)
    {
        return (FnCallResult) { FNCALL_FAILURE, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
    }
    else
    {
        if (!strcmp(detail, "size"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_size);
        }
        else if (!strcmp(detail, "gid"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_gid);
        }
        else if (!strcmp(detail, "uid"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_uid);
        }
        else if (!strcmp(detail, "ino"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_ino);
        }
        else if (!strcmp(detail, "nlink"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_nlink);
        }
        else if (!strcmp(detail, "ctime"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_ctime);
        }
        else if (!strcmp(detail, "mtime"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_mtime);
        }
        else if (!strcmp(detail, "atime"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_atime);
        }
        else if (!strcmp(detail, "permstr"))
        {
        #if !defined(__MINGW32__)
            snprintf(buffer, CF_MAXVARSIZE,
                     "%c%c%c%c%c%c%c%c%c%c",
                     S_ISDIR(statbuf.st_mode) ? 'd' : '-',
                     (statbuf.st_mode & S_IRUSR) ? 'r' : '-',
                     (statbuf.st_mode & S_IWUSR) ? 'w' : '-',
                     (statbuf.st_mode & S_IXUSR) ? 'x' : '-',
                     (statbuf.st_mode & S_IRGRP) ? 'r' : '-',
                     (statbuf.st_mode & S_IWGRP) ? 'w' : '-',
                     (statbuf.st_mode & S_IXGRP) ? 'x' : '-',
                     (statbuf.st_mode & S_IROTH) ? 'r' : '-',
                     (statbuf.st_mode & S_IWOTH) ? 'w' : '-',
                     (statbuf.st_mode & S_IXOTH) ? 'x' : '-');
        #else
            snprintf(buffer, CF_MAXVARSIZE, "Not available on Windows");
        #endif
        }
        else if (!strcmp(detail, "permoct"))
        {
        #if !defined(__MINGW32__)
            snprintf(buffer, CF_MAXVARSIZE, "%jo", (uintmax_t) (statbuf.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)));
        #else
            snprintf(buffer, CF_MAXVARSIZE, "Not available on Windows");
        #endif
        }
        else if (!strcmp(detail, "modeoct"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jo", (uintmax_t) statbuf.st_mode);
        }
        else if (!strcmp(detail, "mode"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_mode);
        }
        else if (!strcmp(detail, "type"))
        {
        #if !defined(__MINGW32__)
            switch (statbuf.st_mode & S_IFMT)
            {
            case S_IFBLK:  snprintf(buffer, CF_MAXVARSIZE, "%s", "block device");     break;
            case S_IFCHR:  snprintf(buffer, CF_MAXVARSIZE, "%s", "character device"); break;
            case S_IFDIR:  snprintf(buffer, CF_MAXVARSIZE, "%s", "directory");        break;
            case S_IFIFO:  snprintf(buffer, CF_MAXVARSIZE, "%s", "FIFO/pipe");        break;
            case S_IFLNK:  snprintf(buffer, CF_MAXVARSIZE, "%s", "symlink");          break;
            case S_IFREG:  snprintf(buffer, CF_MAXVARSIZE, "%s", "regular file");     break;
            case S_IFSOCK: snprintf(buffer, CF_MAXVARSIZE, "%s", "socket");           break;
            default:       snprintf(buffer, CF_MAXVARSIZE, "%s", "unknown");          break;
            }
        #else
            snprintf(buffer, CF_MAXVARSIZE, "Not available on Windows");
        #endif
        }
        else if (!strcmp(detail, "dev_minor"))
        {
        #if !defined(__MINGW32__)
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) minor(statbuf.st_dev) );
        #else
            snprintf(buffer, CF_MAXVARSIZE, "Not available on Windows");
        #endif
        }
        else if (!strcmp(detail, "dev_major"))
        {
        #if !defined(__MINGW32__)
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) major(statbuf.st_dev) );
        #else
            snprintf(buffer, CF_MAXVARSIZE, "Not available on Windows");
        #endif
        }
        else if (!strcmp(detail, "devno"))
        {
        #if !defined(__MINGW32__)
            snprintf(buffer, CF_MAXVARSIZE, "%jd", (uintmax_t) statbuf.st_dev );
        #else
            snprintf(buffer, CF_MAXVARSIZE, "%c:", statbuf.st_dev + 'A');
        #endif
        }
        else if (!strcmp(detail, "dirname"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%s", path);
            ChopLastNode(buffer);
        }
        else if (!strcmp(detail, "basename"))
        {
            snprintf(buffer, CF_MAXVARSIZE, "%s", ReadLastNode(path));
        }
        else if (!strcmp(detail, "linktarget") || !strcmp(detail, "linktarget_shallow"))
        {
#if !defined(__MINGW32__)
            char path_buffer[CF_BUFSIZE];
            bool recurse = !strcmp(detail, "linktarget");
            int cycles = 0;
            int max_cycles = 30; // This allows for up to 31 levels of indirection.

            snprintf(path_buffer, CF_MAXVARSIZE, "%s", path);

            // Iterate while we're looking at a link.
            while (S_ISLNK(statbuf.st_mode))
            {
                if (cycles > max_cycles)
                {
                    Log(LOG_LEVEL_INFO, "%s bailing on link '%s' (original '%s') because %d cycles were chased",
                        fp->name, path_buffer, path, cycles+1);
                    break;
                }

                Log(LOG_LEVEL_VERBOSE, "%s resolving link '%s', cycle %d", fp->name, path_buffer, cycles+1);
                // Prep buffer because readlink() doesn't terminate the path.
                memset(buffer, 0, CF_BUFSIZE);

                /* Note we subtract 1 since we may need an extra char for NULL. */
                if (readlink(path_buffer, buffer, CF_BUFSIZE-1) < 0)
                {
                    // An error happened.  Empty the buffer (don't keep the last target).
                    Log(LOG_LEVEL_ERR, "%s could not readlink '%s'", fp->name, path_buffer);
                    path_buffer[0] = '\0';
                    break;
                }

                Log(LOG_LEVEL_VERBOSE, "%s resolved link '%s' to %s", fp->name, path_buffer, buffer);
                // We got a good link target into buffer.  Copy it to path_buffer.
                snprintf(path_buffer, CF_MAXVARSIZE, "%s", buffer);

                if (!recurse || lstat(path_buffer, &statbuf) == -1)
                {
                    if (!recurse)
                    {
                        Log(LOG_LEVEL_VERBOSE, "%s bailing on link '%s' (original '%s') because linktarget_shallow was requested",
                            fp->name, path_buffer, path);
                    }
                    else // error from lstat
                    {
                        Log(LOG_LEVEL_INFO, "%s bailing on link '%s' (original '%s') because it could not be read",
                            fp->name, path_buffer, path);
                    }
                    break;
                }

                // At this point we haven't bailed, path_buffer has the link target
                cycles++;
            }

            // Get the path_buffer back into buffer.
            snprintf(buffer, CF_MAXVARSIZE, "%s", path_buffer);

#else
            // Always return the original path on W32.
            snprintf(buffer, CF_MAXVARSIZE, "%s", path);
#endif
        }
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallFindfiles(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *returnlist = NULL;
    Rlist *arg = NULL;
    int argcount = 0;
    char id[CF_BUFSIZE];

    snprintf(id, CF_BUFSIZE, "built-in FnCall findfiles-arg");

    /* We need to check all the arguments, ArgTemplate does not check varadic functions */
    for (arg = finalargs; arg; arg = arg->next)
    {
        SyntaxTypeMatch err = CheckConstraintTypeMatch(id, arg->val, DATA_TYPE_STRING, "", 1);
        if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
        {
            FatalError(ctx, "in %s: %s", id, SyntaxTypeMatchToString(err));
        }
        argcount++;
    }

    for (arg = finalargs;  /* Start with arg set to finalargs. */
         arg;              /* We must have arg to proceed. */
         arg = arg->next)  /* arg steps forward every time. */
    {
        char *pattern = RlistScalarValue(arg);
#ifdef __MINGW32__
        RlistAppendScalarIdemp(&returnlist, xstrdup(pattern));
#else /* !__MINGW32__ */
        glob_t globbuf;
        if (0 == glob(pattern, 0, NULL, &globbuf))
        {
            for (int i = 0; i < globbuf.gl_pathc; i++)
            {
                char* found = globbuf.gl_pathv[i];
                char fname[CF_BUFSIZE];
                snprintf(fname, CF_BUFSIZE, "%s", found);
                Log(LOG_LEVEL_VERBOSE, "%s pattern '%s' found match '%s'", fp->name, pattern, fname);
                RlistAppendScalarIdemp(&returnlist, xstrdup(fname));
            }

            globfree(&globbuf);
        }
#endif
    }

    // When no entries were found, mark the empty list
    if (NULL == returnlist)
    {
        RlistAppendScalar(&returnlist, CF_NULL_VALUE);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { returnlist, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallFilter(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    return FilterInternal(ctx,
                          fp,
                          RlistScalarValue(finalargs), // regex or string
                          RlistScalarValue(finalargs->next), // list identifier
                          BooleanFromString(RlistScalarValue(finalargs->next->next)), // match as regex or exactly
                          BooleanFromString(RlistScalarValue(finalargs->next->next->next)), // invert matches
                          IntFromString(RlistScalarValue(finalargs->next->next->next->next))); // max results
}

/*********************************************************************/

static bool GetListReferenceArgument(const EvalContext *ctx, const FnCall *fp, const char *lval_str, Rval *rval_out, DataType *datatype_out)
{
    VarRef *ref = VarRefParse(lval_str);

    if (!EvalContextVariableGet(ctx, ref, rval_out, datatype_out))
    {
        Log(LOG_LEVEL_INFO, "Could not resolve expected list variable '%s' in function '%s'", lval_str, fp->name);
        VarRefDestroy(ref);
        return false;
    }

    VarRefDestroy(ref);

    if (rval_out->type != RVAL_TYPE_LIST)
    {
        Log(LOG_LEVEL_VERBOSE, "Function '%s' expected a list variable reference, got variable of type '%s'", fp->name, DataTypeToString(*datatype_out));
        return false;
    }

    return true;
}

/*********************************************************************/

static FnCallResult FilterInternal(EvalContext *ctx, FnCall *fp, char *regex, char *name, int do_regex, int invert, long max)
{
    Rval rval2;
    Rlist *returnlist = NULL;

    if (!GetListReferenceArgument(ctx, fp, name, &rval2, NULL))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    RlistAppendScalar(&returnlist, CF_NULL_VALUE);

    long match_count = 0;
    long total = 0;
    for (const Rlist *rp = (const Rlist *) rval2.item; rp != NULL && match_count < max; rp = rp->next)
    {
        bool found = do_regex ? FullTextMatch(ctx, regex, RlistScalarValue(rp)) : (0==strcmp(regex, RlistScalarValue(rp)));

        if (invert ? !found : found)
        {
            RlistAppendScalar(&returnlist, RlistScalarValue(rp));
            match_count++;

            // exit early in case "some" is being called
            if (0 == strcmp(fp->name, "some"))
            {
                break;
            }
        }
        // exit early in case "none" is being called
        else if (0 == strcmp(fp->name, "every"))
        {
            total++; // we just 
            break;
        }

        total++;
    }

    bool contextmode = 0;
    bool ret;
    if (0 == strcmp(fp->name, "every"))
    {
        contextmode = 1;
        ret = (match_count == total);
    }
    else if (0 == strcmp(fp->name, "none"))
    {
        contextmode = 1;
        ret = (match_count == 0);
    }
    else if (0 == strcmp(fp->name, "some"))
    {
        contextmode = 1;
        ret = (match_count > 0);
    }
    else if (0 != strcmp(fp->name, "grep") && 0 != strcmp(fp->name, "filter"))
    {
        ProgrammingError("built-in FnCall %s: unhandled FilterInternal() contextmode", fp->name);
    }

    if (contextmode)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(ret ? "any" : "!any"), RVAL_TYPE_SCALAR } };
    }

    // else, return the list itself
    return (FnCallResult) { FNCALL_SUCCESS, { returnlist, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallSublist(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    const char *name = RlistScalarValue(finalargs); // list identifier
    bool head = 0 == strcmp(RlistScalarValue(finalargs->next), "head"); // heads or tails
    long max = IntFromString(RlistScalarValue(finalargs->next->next)); // max results

    Rval rval2;
    Rlist *returnlist = NULL;

    if (!GetListReferenceArgument(ctx, fp, name, &rval2, NULL))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    RlistAppendScalar(&returnlist, CF_NULL_VALUE);

    if (head)
    {
        long count = 0;
        for (const Rlist *rp = (const Rlist *) rval2.item; rp != NULL && count < max; rp = rp->next)
        {
            RlistAppendScalar(&returnlist, RlistScalarValue(rp));
            count++;
        }
    }
    else if (max > 0) // tail mode
    {
        const Rlist *rp = (const Rlist *) rval2.item;
        int length = RlistLen((const Rlist *) rp);

        int offset = max >= length ? 0 : length-max;

        for (; rp != NULL && offset--; rp = rp->next);

        for (; rp != NULL; rp = rp->next)
        {
            RlistAppendScalar(&returnlist, RlistScalarValue(rp));
        }

    }

    return (FnCallResult) { FNCALL_SUCCESS, { returnlist, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallSetop(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    bool difference = (0 == strcmp(fp->name, "difference"));

    const char *name_a = RlistScalarValue(finalargs);
    const char *name_b = RlistScalarValue(finalargs->next);

    Rval rval_a;
    if (!GetListReferenceArgument(ctx, fp, name_a, &rval_a, NULL))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    Rval rval_b;
    if (!GetListReferenceArgument(ctx, fp, name_b, &rval_b, NULL))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    Rlist *returnlist = NULL;
    RlistAppendScalar(&returnlist, CF_NULL_VALUE);

    StringSet *set_b = StringSetNew();
    for (const Rlist *rp_b = rval_b.item; rp_b != NULL; rp_b = rp_b->next)
    {
        StringSetAdd(set_b, xstrdup(RlistScalarValue(rp_b)));
    }

    for (const Rlist *rp_a = rval_a.item; rp_a != NULL; rp_a = rp_a->next)
    {
        if (strcmp(RlistScalarValue(rp_a), CF_NULL_VALUE) == 0)
        {
            continue;
        }

        // Yes, this is an XOR.  But it's more legible this way.
        if (difference && StringSetContains(set_b, RlistScalarValue(rp_a)))
        {
            continue;
        }

        if (!difference && !StringSetContains(set_b, RlistScalarValue(rp_a)))
        {
            continue;
        }
                
        RlistAppendScalarIdemp(&returnlist, RlistScalarValue(rp_a));
    }

    StringSetDestroy(set_b);

    return (FnCallResult) { FNCALL_SUCCESS, (Rval) { returnlist, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallLength(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    const char *name = RlistScalarValue(finalargs);

    Rval rval2;
    char buffer[CF_BUFSIZE];
    int count = 0;

    if (!GetListReferenceArgument(ctx, fp, name, &rval2, NULL))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    bool null_seen = false;
    for (const Rlist *rp = RvalRlistValue(rval2); rp != NULL; rp = rp->next)
    {
        if (strcmp(RlistScalarValue(rp), CF_NULL_VALUE) == 0)
        {
            null_seen = true;
        }
        count++;
    }

    if (count == 1 && null_seen)
    {
        count = 0;
    }

    snprintf(buffer, CF_MAXVARSIZE, "%d", count);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallUnique(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    const char *name = RlistScalarValue(finalargs);

    Rval rval2;
    Rlist *returnlist = NULL;

    if (!GetListReferenceArgument(ctx, fp, name, &rval2, NULL))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    RlistAppendScalar(&returnlist, CF_NULL_VALUE);

    for (const Rlist *rp = (const Rlist *) rval2.item; rp != NULL; rp = rp->next)
    {
        RlistAppendScalarIdemp(&returnlist, RlistScalarValue(rp));
    }

    return (FnCallResult) { FNCALL_SUCCESS, { returnlist, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallNth(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    const char *name = RlistScalarValue(finalargs);
    long offset = IntFromString(RlistScalarValue(finalargs->next)); // offset

    Rval rval2;

    if (!GetListReferenceArgument(ctx, fp, name, &rval2, NULL))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    const Rlist *rp;
    for (rp = (const Rlist *) rval2.item; rp != NULL && offset--; rp = rp->next);

    if (NULL == rp) return (FnCallResult) { FNCALL_FAILURE };

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(RlistScalarValue(rp)), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallEverySomeNone(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    return FilterInternal(ctx,
                          fp,
                          RlistScalarValue(finalargs), // regex or string
                          RlistScalarValue(finalargs->next), // list identifier
                          1,
                          0,
                          99999999999);
}

static FnCallResult FnCallSort(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    VarRef *ref = VarRefParse(RlistScalarValue(finalargs));
    const char *sort_type = RlistScalarValue(finalargs->next); // list identifier
    Rval list_var_rval;
    DataType list_var_dtype = DATA_TYPE_NONE;

    if (!EvalContextVariableGet(ctx, ref, &list_var_rval, &list_var_dtype))
    {
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    VarRefDestroy(ref);

    if (list_var_dtype != DATA_TYPE_STRING_LIST)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    Rlist *sorted;

    if (strcmp(sort_type, "int") == 0)
    {
        sorted = IntSortRListNames(RlistCopy(RvalRlistValue(list_var_rval)));
    }
    else if (strcmp(sort_type, "real") == 0)
    {
        sorted = RealSortRListNames(RlistCopy(RvalRlistValue(list_var_rval)));
    }
    else if (strcmp(sort_type, "IP") == 0 || strcmp(sort_type, "ip") == 0)
    {
        sorted = IPSortRListNames(RlistCopy(RvalRlistValue(list_var_rval)));
    }
    else if (strcmp(sort_type, "MAC") == 0 || strcmp(sort_type, "mac") == 0)
    {
        sorted = MACSortRListNames(RlistCopy(RvalRlistValue(list_var_rval)));
    }
    else // "lex"
    {
        sorted = AlphaSortRListNames(RlistCopy(RvalRlistValue(list_var_rval)));
    }

    return (FnCallResult) { FNCALL_SUCCESS, (Rval) { sorted, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallFormat(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char id[CF_BUFSIZE];

    snprintf(id, CF_BUFSIZE, "built-in FnCall %s-arg", fp->name);

/* We need to check all the arguments, ArgTemplate does not check varadic functions */
    for (const Rlist *arg = finalargs; arg; arg = arg->next)
    {
        SyntaxTypeMatch err = CheckConstraintTypeMatch(id, arg->val, DATA_TYPE_STRING, "", 1);
        if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
        {
            FatalError(ctx, "in %s: %s", id, SyntaxTypeMatchToString(err));
        }
    }

/* begin fn specific content */
    if (!finalargs)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    char *format = RlistScalarValue(finalargs);

    if (!format)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    const Rlist *rp = finalargs->next;

    char *check = strchr(format, '%');
    char check_buffer[CF_BUFSIZE];
    Buffer *buf = BufferNew();

    if (check)
    {
        BufferAppend(buf, format, (check - format));

        while (check && FullTextMatch(ctx, "(%%|%[^diouxXeEfFgGaAcsCSpnm%]*?[diouxXeEfFgGaAcsCSpnm])([^%]*)(.*)", check))
        {
            {
                VarRef *ref_1 = VarRefParseFromScope("1", "match");
                Rval rval_1;
                DataType type_1 = DATA_TYPE_NONE;
                if (EvalContextVariableGet(ctx, ref_1, &rval_1, &type_1))
                {
                    const char* format_piece = RvalScalarValue(rval_1);
                    bool percent = (0 == strncmp(format_piece, "%%", 2));
                    char *data = NULL;

                    if (percent)
                    {
                    }
                    else if (rp)
                    {
                        data = RlistScalarValue(rp);
                        rp = rp->next;
                    }
                    else // not %% and no data
                    {
                        Log(LOG_LEVEL_ERR, "format() didn't have enough parameters");
                        BufferDestroy(&buf);
                        return (FnCallResult) { FNCALL_FAILURE };
                    }

                    char piece[CF_BUFSIZE];
                    memset(piece, 0, CF_BUFSIZE);

                    // CfOut(OUTPUT_LEVEL_INFORM, "", "format: processing format piece = '%s' with data '%s'", format_piece, percent ? "%" : data);

                    char bad_modifiers[] = "hLqjzt";
                    for (int b = 0; b < strlen(bad_modifiers); b++)
                    {
                        if (NULL != strchr(format_piece, bad_modifiers[b]))
                        {
                            Log(LOG_LEVEL_ERR, "format() does not allow modifier character '%c' in format specifier '%s'.",
                                  bad_modifiers[b],
                                  format_piece);
                            BufferDestroy(&buf);
                            return (FnCallResult) { FNCALL_FAILURE };
                        }
                    }

                    if (strrchr(format_piece, 'd') || strrchr(format_piece, 'o') || strrchr(format_piece, 'x'))
                    {
                        long x = 0;
                        sscanf(data, "%ld%s", &x, piece); // we don't care about the remainder and will overwrite it
                        snprintf(piece, CF_BUFSIZE, format_piece, x);
                        BufferAppend(buf, piece, strlen(piece));
                        // CfOut(OUTPUT_LEVEL_INFORM, "", "format: appending int format piece = '%s' with data '%s'", format_piece, data);
                    }
                    else if (percent)
                    {
                        BufferAppend(buf, "%", 1);
                        // CfOut(OUTPUT_LEVEL_INFORM, "", "format: appending int format piece = '%s' with data '%s'", format_piece, data);
                    }
                    else if (strrchr(format_piece, 'f'))
                    {
                        double x = 0;
                        sscanf(data, "%lf%s", &x, piece); // we don't care about the remainder and will overwrite it
                        snprintf(piece, CF_BUFSIZE, format_piece, x);
                        BufferAppend(buf, piece, strlen(piece));
                        // CfOut(OUTPUT_LEVEL_INFORM, "", "format: appending float format piece = '%s' with data '%s'", format_piece, data);
                    }
                    else if (strrchr(format_piece, 's'))
                    {
                        snprintf(piece, CF_BUFSIZE, format_piece, data);
                        BufferAppend(buf, piece, strlen(piece));
                        // CfOut(OUTPUT_LEVEL_INFORM, "", "format: appending string format piece = '%s' with data '%s'", format_piece, data);
                    }
                    else
                    {
                        char error[] = "(unhandled format)";
                        BufferAppend(buf, error, strlen(error));
                        // CfOut(OUTPUT_LEVEL_INFORM, "", "format: error appending unhandled format piece = '%s' with data '%s'", format_piece, data);
                    }
                }
                else
                {
                    check = NULL;
                }

                VarRefDestroy(ref_1);
            }

            {
                VarRef *ref_2 = VarRefParseFromScope("2", "match");
                Rval rval_2;
                DataType type_2 = DATA_TYPE_NONE;
                if (EvalContextVariableGet(ctx, ref_2, &rval_2, &type_2))
                {
                    const char* static_piece = RvalScalarValue(rval_2);
                    BufferAppend(buf, static_piece, strlen(static_piece));
                    // CfOut(OUTPUT_LEVEL_INFORM, "", "format: appending static piece = '%s'", static_piece);
                }
                else
                {
                    check = NULL;
                }

                VarRefDestroy(ref_2);
            }

            {
                VarRef *ref_3 = VarRefParseFromScope("3", "match");
                Rval rval_3;
                DataType type_3 = DATA_TYPE_NONE;
                if (EvalContextVariableGet(ctx, ref_3, &rval_3, &type_3))
                {
                    strncpy(check_buffer, RvalScalarValue(rval_3), CF_BUFSIZE);
                    check = check_buffer;
                }
                else
                {
                    check = NULL;
                }

                VarRefDestroy(ref_3);
            }
        }
    }
    else
    {
        BufferAppend(buf, format, strlen(format));
    }

    char result[CF_BUFSIZE] = "";
    memset(result, 0, CF_BUFSIZE);
    strncpy(result, BufferData(buf), CF_BUFSIZE);
    BufferDestroy(&buf);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(result), RVAL_TYPE_SCALAR } };

}

/*********************************************************************/

static FnCallResult FnCallIPRange(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE], *range = RlistScalarValue(finalargs);
    Item *ip;

    buffer[0] = '\0';

/* begin fn specific content */

    strcpy(buffer, "!any");

    if (!FuzzyMatchParse(range))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    for (ip = IPADDRESSES; ip != NULL; ip = ip->next)
    {
        if (FuzzySetMatch(range, VIPADDRESS) == 0)
        {
            strcpy(buffer, "any");
            break;
        }
        else
        {
            if (FuzzySetMatch(range, ip->name) == 0)
            {
                strcpy(buffer, "any");
                break;
            }
        }
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallHostRange(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];

    buffer[0] = '\0';

/* begin fn specific content */

    char *prefix = RlistScalarValue(finalargs);
    char *range = RlistScalarValue(finalargs->next);

    strcpy(buffer, "!any");

    if (!FuzzyHostParse(range))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (FuzzyHostMatch(prefix, range, VUQNAME) == 0)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
    }
}

/*********************************************************************/

FnCallResult FnCallHostInNetgroup(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];
    char *host, *user, *domain;

    buffer[0] = '\0';

/* begin fn specific content */

    strcpy(buffer, "!any");

    setnetgrent(RlistScalarValue(finalargs));

    while (getnetgrent(&host, &user, &domain))
    {
        if (host == NULL)
        {
            Log(LOG_LEVEL_VERBOSE, "Matched '%s' in netgroup '%s'", VFQNAME, RlistScalarValue(finalargs));
            strcpy(buffer, "any");
            break;
        }

        if (strcmp(host, VFQNAME) == 0 || strcmp(host, VUQNAME) == 0)
        {
            Log(LOG_LEVEL_VERBOSE, "Matched '%s' in netgroup '%s'", host, RlistScalarValue(finalargs));
            strcpy(buffer, "any");
            break;
        }
    }

    endnetgrent();

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallIsVariable(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    const char *lval = RlistScalarValue(finalargs);
    Rval rval = { 0 };
    bool found = false;

    if (!lval)
    {
        found = false;
    }
    else
    {
        VarRef *ref = VarRefParse(lval);
        found = EvalContextVariableGet(ctx, ref, &rval, NULL);
        VarRefDestroy(ref);
    }

    if (found)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
    }
}

/*********************************************************************/

static FnCallResult FnCallStrCmp(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    if (strcmp(RlistScalarValue(finalargs), RlistScalarValue(finalargs->next)) == 0)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
    }
}

/*********************************************************************/

static FnCallResult FnCallTranslatePath(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[MAX_FILENAME];

    buffer[0] = '\0';

/* begin fn specific content */

    snprintf(buffer, sizeof(buffer), "%s", RlistScalarValue(finalargs));
    MapName(buffer);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

#if defined(__MINGW32__)

static FnCallResult FnCallRegistryValue(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE] = "";

    if (GetRegistryValue(RlistScalarValue(finalargs), RlistScalarValue(finalargs->next), buffer, sizeof(buffer)))
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
    return (FnCallResult) { FNCALL_FAILURE };
}

#else /* !__MINGW32__ */

static FnCallResult FnCallRegistryValue(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    return (FnCallResult) { FNCALL_FAILURE };
}

#endif /* !__MINGW32__ */

/*********************************************************************/

static FnCallResult FnCallRemoteScalar(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];

    buffer[0] = '\0';

/* begin fn specific content */

    char *handle = RlistScalarValue(finalargs);
    char *server = RlistScalarValue(finalargs->next);
    int encrypted = BooleanFromString(RlistScalarValue(finalargs->next->next));

    if (strcmp(server, "localhost") == 0)
    {
        /* The only reason for this is testing... */
        server = "127.0.0.1";
    }

    if (THIS_AGENT_TYPE == AGENT_TYPE_COMMON)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("<remote scalar>"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        GetRemoteScalar(ctx, "VAR", handle, server, encrypted, buffer);

        if (strncmp(buffer, "BAD:", 4) == 0)
        {
            if (!RetrieveUnreliableValue("remotescalar", handle, buffer))
            {
                // This function should never fail
                buffer[0] = '\0';
            }
        }
        else
        {
            CacheUnreliableValue("remotescalar", handle, buffer);
        }

        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
    }
}

/*********************************************************************/

static FnCallResult FnCallHubKnowledge(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];

    buffer[0] = '\0';

/* begin fn specific content */

    char *handle = RlistScalarValue(finalargs);

    if (THIS_AGENT_TYPE != AGENT_TYPE_AGENT)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("<inaccessible remote scalar>"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Accessing hub knowledge base for '%s'", handle);
        GetRemoteScalar(ctx, "VAR", handle, POLICY_SERVER, true, buffer);

        // This should always be successful - and this one doesn't cache

        if (strncmp(buffer, "BAD:", 4) == 0)
        {
            snprintf(buffer, CF_MAXVARSIZE, "0");
        }

        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
    }
}

/*********************************************************************/

static FnCallResult FnCallRemoteClassesMatching(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp, *classlist;
    char buffer[CF_BUFSIZE], class[CF_MAXVARSIZE];

    buffer[0] = '\0';

/* begin fn specific content */

    char *regex = RlistScalarValue(finalargs);
    char *server = RlistScalarValue(finalargs->next);
    int encrypted = BooleanFromString(RlistScalarValue(finalargs->next->next));
    char *prefix = RlistScalarValue(finalargs->next->next->next);

    if (strcmp(server, "localhost") == 0)
    {
        /* The only reason for this is testing... */
        server = "127.0.0.1";
    }

    if (THIS_AGENT_TYPE == AGENT_TYPE_COMMON)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("remote_classes"), RVAL_TYPE_SCALAR } };
    }
    else
    {
        GetRemoteScalar(ctx, "CONTEXT", regex, server, encrypted, buffer);

        if (strncmp(buffer, "BAD:", 4) == 0)
        {
            return (FnCallResult) { FNCALL_FAILURE };
        }

        if ((classlist = RlistFromSplitString(buffer, ',')))
        {
            for (rp = classlist; rp != NULL; rp = rp->next)
            {
                snprintf(class, CF_MAXVARSIZE - 1, "%s_%s", prefix, RlistScalarValue(rp));
                EvalContextClassPut(ctx, NULL, class, true, CONTEXT_SCOPE_BUNDLE);
            }
            RlistDestroy(classlist);
        }

        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
    }
}

/*********************************************************************/

static FnCallResult FnCallPeers(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp, *newlist, *pruned;
    char *split = "\n";
    char *file_buffer = NULL;
    int i, found, maxent = 100000, maxsize = 100000;

/* begin fn specific content */

    char *filename = RlistScalarValue(finalargs);
    char *comment = RlistScalarValue(finalargs->next);
    int groupsize = IntFromString(RlistScalarValue(finalargs->next->next));

    file_buffer = (char *) CfReadFile(filename, maxsize);

    if (file_buffer == NULL)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    file_buffer = StripPatterns(ctx, file_buffer, comment, filename);

    if (file_buffer == NULL)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { NULL, RVAL_TYPE_LIST } };
    }
    else
    {
        newlist = RlistFromSplitRegex(ctx, file_buffer, split, maxent, true);
    }

/* Slice up the list and discard everything except our slice */

    i = 0;
    found = false;
    pruned = NULL;

    for (rp = newlist; rp != NULL; rp = rp->next)
    {
        char s[CF_MAXVARSIZE];

        if (EmptyString(RlistScalarValue(rp)))
        {
            continue;
        }

        s[0] = '\0';
        sscanf(RlistScalarValue(rp), "%s", s);

        if (strcmp(s, VFQNAME) == 0 || strcmp(s, VUQNAME) == 0)
        {
            found = true;
        }
        else
        {
            RlistPrepend(&pruned, s, RVAL_TYPE_SCALAR);
        }

        if (i++ % groupsize == groupsize - 1)
        {
            if (found)
            {
                break;
            }
            else
            {
                RlistDestroy(pruned);
                pruned = NULL;
            }
        }
    }

    RlistDestroy(newlist);
    free(file_buffer);

    if (pruned)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { pruned, RVAL_TYPE_LIST } };
    }
    else
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
}

/*********************************************************************/

static FnCallResult FnCallPeerLeader(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp, *newlist;
    char *split = "\n";
    char *file_buffer = NULL, buffer[CF_MAXVARSIZE];
    int i, found, maxent = 100000, maxsize = 100000;

    buffer[0] = '\0';

/* begin fn specific content */

    char *filename = RlistScalarValue(finalargs);
    char *comment = RlistScalarValue(finalargs->next);
    int groupsize = IntFromString(RlistScalarValue(finalargs->next->next));

    file_buffer = (char *) CfReadFile(filename, maxsize);

    if (file_buffer == NULL)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
    else
    {
        file_buffer = StripPatterns(ctx, file_buffer, comment, filename);

        if (file_buffer == NULL)
        {
            return (FnCallResult) { FNCALL_SUCCESS, { NULL, RVAL_TYPE_LIST } };
        }
        else
        {
            newlist = RlistFromSplitRegex(ctx, file_buffer, split, maxent, true);
        }
    }

/* Slice up the list and discard everything except our slice */

    i = 0;
    found = false;
    buffer[0] = '\0';

    for (rp = newlist; rp != NULL; rp = rp->next)
    {
        char s[CF_MAXVARSIZE];

        if (EmptyString(RlistScalarValue(rp)))
        {
            continue;
        }

        s[0] = '\0';
        sscanf(RlistScalarValue(rp), "%s", s);

        if (strcmp(s, VFQNAME) == 0 || strcmp(s, VUQNAME) == 0)
        {
            found = true;
        }

        if (i % groupsize == 0)
        {
            if (found)
            {
                if (strcmp(s, VFQNAME) == 0 || strcmp(s, VUQNAME) == 0)
                {
                    strncpy(buffer, "localhost", CF_MAXVARSIZE - 1);
                }
                else
                {
                    strncpy(buffer, s, CF_MAXVARSIZE - 1);
                }
                break;
            }
        }

        i++;
    }

    RlistDestroy(newlist);
    free(file_buffer);

    if (strlen(buffer) > 0)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

}

/*********************************************************************/

static FnCallResult FnCallPeerLeaders(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp, *newlist, *pruned;
    char *split = "\n";
    char *file_buffer = NULL;
    int i, maxent = 100000, maxsize = 100000;

/* begin fn specific content */

    char *filename = RlistScalarValue(finalargs);
    char *comment = RlistScalarValue(finalargs->next);
    int groupsize = IntFromString(RlistScalarValue(finalargs->next->next));

    file_buffer = (char *) CfReadFile(filename, maxsize);

    if (file_buffer == NULL)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    file_buffer = StripPatterns(ctx, file_buffer, comment, filename);

    if (file_buffer == NULL)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { NULL, RVAL_TYPE_LIST } };
    }

    newlist = RlistFromSplitRegex(ctx, file_buffer, split, maxent, true);

/* Slice up the list and discard everything except our slice */

    i = 0;
    pruned = NULL;

    for (rp = newlist; rp != NULL; rp = rp->next)
    {
        char s[CF_MAXVARSIZE];

        if (EmptyString(RlistScalarValue(rp)))
        {
            continue;
        }

        s[0] = '\0';
        sscanf(RlistScalarValue(rp), "%s", s);

        if (i % groupsize == 0)
        {
            if (strcmp(s, VFQNAME) == 0 || strcmp(s, VUQNAME) == 0)
            {
                RlistPrepend(&pruned, "localhost", RVAL_TYPE_SCALAR);
            }
            else
            {
                RlistPrepend(&pruned, s, RVAL_TYPE_SCALAR);
            }
        }

        i++;
    }

    RlistDestroy(newlist);
    free(file_buffer);

    if (pruned)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { pruned, RVAL_TYPE_LIST } };
    }
    else
    {
        free(file_buffer);
        return (FnCallResult) { FNCALL_FAILURE };
    }

}

/*********************************************************************/

static FnCallResult FnCallRegCmp(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];

    buffer[0] = '\0';

/* begin fn specific content */

    strcpy(buffer, CF_ANYCLASS);
    char *argv0 = RlistScalarValue(finalargs);
    char *argv1 = RlistScalarValue(finalargs->next);

    if (FullTextMatch(ctx, argv0, argv1))
    {
        strcpy(buffer, "any");
    }
    else
    {
        strcpy(buffer, "!any");
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallRegExtract(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];

    buffer[0] = '\0';

/* begin fn specific content */

    strcpy(buffer, CF_ANYCLASS);
    char *regex = RlistScalarValue(finalargs);
    char *data = RlistScalarValue(finalargs->next);
    char *arrayname = RlistScalarValue(finalargs->next->next);

    if (FullTextMatch(ctx, regex, data))
    {
        strcpy(buffer, "any");
    }
    else
    {
        strcpy(buffer, "!any");
    }

    long i = 0;

    while (true)
    {
        Rval rval;
        DataType type;

        char *index = StringFromLong(i);
        VarRef *ref = VarRefParseFromScope(index, "match");
        free(index);

        if (!EvalContextVariableGet(ctx, ref, &rval, &type))
        {
            break;
        }

        if (rval.type != RVAL_TYPE_SCALAR)
        {
            Log(LOG_LEVEL_ERR,
                  "Software error: pattern match was non-scalar in regextract (shouldn't happen)");
            return (FnCallResult) { FNCALL_FAILURE };
        }
        else
        {
            char var[CF_MAXVARSIZE] = "";
            snprintf(var, CF_MAXVARSIZE - 1, "%s[%s]", arrayname, ref->lval);
            VarRef *new_ref = VarRefParseFromBundle(var, PromiseGetBundle(fp->caller));
            EvalContextVariablePut(ctx, new_ref, RvalScalarValue(rval), DATA_TYPE_STRING);
            VarRefDestroy(new_ref);
        }

        i++;
    }

    if (i == 0)
    {
        strcpy(buffer, "!any");
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallRegLine(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];
    FILE *fin;

    buffer[0] = '\0';

/* begin fn specific content */

    char *argv0 = RlistScalarValue(finalargs);
    char *argv1 = RlistScalarValue(finalargs->next);

    strcpy(buffer, "!any");

    if ((fin = fopen(argv1, "r")) != NULL)
    {
        for (;;)
        {
            char line[CF_BUFSIZE];

            if (fgets(line, sizeof(line), fin) == NULL)
            {
                if (ferror(fin))
                {
                    Log(LOG_LEVEL_ERR, "Function regline, unable to read from the file '%s'", argv1);
                    fclose(fin);
                    return (FnCallResult) { FNCALL_FAILURE };
                }
                else /* feof */
                {
                    break;
                }
            }

            if (Chop(line, CF_EXPANDSIZE) == -1)
            {
                Log(LOG_LEVEL_ERR, "Chop was called on a string that seemed to have no terminator");
            }

            if (FullTextMatch(ctx, argv0, line))
            {
                strcpy(buffer, "any");
                break;
            }
        }

        fclose(fin);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallIsLessGreaterThan(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];

    buffer[0] = '\0';

    char *argv0 = RlistScalarValue(finalargs);
    char *argv1 = RlistScalarValue(finalargs->next);

    if (IsRealNumber(argv0) && IsRealNumber(argv1))
    {
        double a = 0;
        if (!DoubleFromString(argv0, &a))
        {
            return (FnCallResult) { FNCALL_FAILURE };
        }
        double b = 0;
        if (!DoubleFromString(argv1, &b))
        {
            return (FnCallResult) { FNCALL_FAILURE };
        }

        if (!strcmp(fp->name, "isgreaterthan"))
        {
            if (a > b)
            {
                strcpy(buffer, "any");
            }
            else
            {
                strcpy(buffer, "!any");
            }
        }
        else
        {
            if (a < b)
            {
                strcpy(buffer, "any");
            }
            else
            {
                strcpy(buffer, "!any");
            }
        }
    }
    else if (strcmp(argv0, argv1) > 0)
    {
        if (!strcmp(fp->name, "isgreaterthan"))
        {
            strcpy(buffer, "any");
        }
        else
        {
            strcpy(buffer, "!any");
        }
    }
    else
    {
        if (!strcmp(fp->name, "isgreaterthan"))
        {
            strcpy(buffer, "!any");
        }
        else
        {
            strcpy(buffer, "any");
        }
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallIRange(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];
    long tmp;

    buffer[0] = '\0';

/* begin fn specific content */

    long from = IntFromString(RlistScalarValue(finalargs));
    long to = IntFromString(RlistScalarValue(finalargs->next));

    if (from == CF_NOINT || to == CF_NOINT)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (from > to)
    {
        tmp = to;
        to = from;
        from = tmp;
    }

    snprintf(buffer, CF_BUFSIZE - 1, "%ld,%ld", from, to);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallRRange(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];
    int tmp;

    buffer[0] = '\0';

/* begin fn specific content */

    double from = 0;
    if (!DoubleFromString(RlistScalarValue(finalargs), &from))
    {
        Log(LOG_LEVEL_ERR, "Function rrange, error reading assumed real value '%s' => %lf", RlistScalarValue(finalargs), from);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    double to = 0;
    if (!DoubleFromString(RlistScalarValue(finalargs), &to))
    {
        Log(LOG_LEVEL_ERR, "Function rrange, error reading assumed real value '%s' => %lf", RlistScalarValue(finalargs->next), from);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (from > to)
    {
        tmp = to;
        to = from;
        from = tmp;
    }

    snprintf(buffer, CF_BUFSIZE - 1, "%lf,%lf", from, to);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

static FnCallResult FnCallReverse(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rval list_rval;
    DataType list_dtype = DATA_TYPE_NONE;

    if (!GetListReferenceArgument(ctx, fp, RlistScalarValue(finalargs), &list_rval, &list_dtype))
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

    if (list_dtype != DATA_TYPE_STRING_LIST)
    {
        Log(LOG_LEVEL_ERR, "Function '%s' expected a variable that resolves to a string list, got '%s'", fp->name, DataTypeToString(list_dtype));
        return (FnCallResult) { FNCALL_FAILURE };
    }

    Rlist *copy = RlistCopy(RvalRlistValue(list_rval));
    RlistReverse(&copy);

    return (FnCallResult) { FNCALL_SUCCESS, (Rval) { copy, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallOn(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp;
    char buffer[CF_BUFSIZE];
    long d[6];
    time_t cftime;
    struct tm tmv;
    DateTemplate i;

    buffer[0] = '\0';

/* begin fn specific content */

    rp = finalargs;

    for (i = 0; i < 6; i++)
    {
        if (rp != NULL)
        {
            d[i] = IntFromString(RlistScalarValue(rp));
            rp = rp->next;
        }
    }

/* (year,month,day,hour,minutes,seconds) */

    tmv.tm_year = d[DATE_TEMPLATE_YEAR] - 1900;
    tmv.tm_mon = d[DATE_TEMPLATE_MONTH] - 1;
    tmv.tm_mday = d[DATE_TEMPLATE_DAY];
    tmv.tm_hour = d[DATE_TEMPLATE_HOUR];
    tmv.tm_min = d[DATE_TEMPLATE_MIN];
    tmv.tm_sec = d[DATE_TEMPLATE_SEC];
    tmv.tm_isdst = -1;

    if ((cftime = mktime(&tmv)) == -1)
    {
        Log(LOG_LEVEL_INFO, "Illegal time value");
    }

    snprintf(buffer, CF_BUFSIZE - 1, "%ld", cftime);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallOr(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *arg;
    char id[CF_BUFSIZE];

    snprintf(id, CF_BUFSIZE, "built-in FnCall or-arg");

/* We need to check all the arguments, ArgTemplate does not check varadic functions */
    for (arg = finalargs; arg; arg = arg->next)
    {
        SyntaxTypeMatch err = CheckConstraintTypeMatch(id, arg->val, DATA_TYPE_STRING, "", 1);
        if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
        {
            FatalError(ctx, "in %s: %s", id, SyntaxTypeMatchToString(err));
        }
    }

    for (arg = finalargs; arg; arg = arg->next)
    {
        if (IsDefinedClass(ctx, RlistScalarValue(arg), PromiseGetNamespace(fp->caller)))
        {
            return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("any"), RVAL_TYPE_SCALAR } };
        }
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup("!any"), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallLaterThan(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp;
    char buffer[CF_BUFSIZE];
    long d[6];
    time_t cftime, now = time(NULL);
    struct tm tmv;
    DateTemplate i;

    buffer[0] = '\0';

/* begin fn specific content */

    rp = finalargs;

    for (i = 0; i < 6; i++)
    {
        if (rp != NULL)
        {
            d[i] = IntFromString(RlistScalarValue(rp));
            rp = rp->next;
        }
    }

/* (year,month,day,hour,minutes,seconds) */

    tmv.tm_year = d[DATE_TEMPLATE_YEAR] - 1900;
    tmv.tm_mon = d[DATE_TEMPLATE_MONTH] - 1;
    tmv.tm_mday = d[DATE_TEMPLATE_DAY];
    tmv.tm_hour = d[DATE_TEMPLATE_HOUR];
    tmv.tm_min = d[DATE_TEMPLATE_MIN];
    tmv.tm_sec = d[DATE_TEMPLATE_SEC];
    tmv.tm_isdst = -1;

    if ((cftime = mktime(&tmv)) == -1)
    {
        Log(LOG_LEVEL_INFO, "Illegal time value");
    }

    if (now > cftime)
    {
        strcpy(buffer, CF_ANYCLASS);
    }
    else
    {
        strcpy(buffer, "!any");
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallAgoDate(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp;
    char buffer[CF_BUFSIZE];
    time_t cftime;
    long d[6];
    DateTemplate i;

    buffer[0] = '\0';

/* begin fn specific content */

    rp = finalargs;

    for (i = 0; i < 6; i++)
    {
        if (rp != NULL)
        {
            d[i] = IntFromString(RlistScalarValue(rp));
            rp = rp->next;
        }
    }

/* (year,month,day,hour,minutes,seconds) */

    cftime = CFSTARTTIME;
    cftime -= d[DATE_TEMPLATE_SEC];
    cftime -= d[DATE_TEMPLATE_MIN] * 60;
    cftime -= d[DATE_TEMPLATE_HOUR] * 3600;
    cftime -= d[DATE_TEMPLATE_DAY] * 24 * 3600;
    cftime -= Months2Seconds(d[DATE_TEMPLATE_MONTH]);
    cftime -= d[DATE_TEMPLATE_YEAR] * 365 * 24 * 3600;

    snprintf(buffer, CF_BUFSIZE - 1, "%ld", cftime);

    if (cftime < 0)
    {
        strcpy(buffer, "0");
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallAccumulatedDate(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp;
    char buffer[CF_BUFSIZE];
    long d[6], cftime;
    DateTemplate i;

    buffer[0] = '\0';

/* begin fn specific content */

    rp = finalargs;

    for (i = 0; i < 6; i++)
    {
        if (rp != NULL)
        {
            d[i] = IntFromString(RlistScalarValue(rp));
            rp = rp->next;
        }
    }

/* (year,month,day,hour,minutes,seconds) */

    cftime = 0;
    cftime += d[DATE_TEMPLATE_SEC];
    cftime += d[DATE_TEMPLATE_MIN] * 60;
    cftime += d[DATE_TEMPLATE_HOUR] * 3600;
    cftime += d[DATE_TEMPLATE_DAY] * 24 * 3600;
    cftime += d[DATE_TEMPLATE_MONTH] * 30 * 24 * 3600;
    cftime += d[DATE_TEMPLATE_YEAR] * 365 * 24 * 3600;

    snprintf(buffer, CF_BUFSIZE - 1, "%ld", cftime);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallNot(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(IsDefinedClass(ctx, RlistScalarValue(finalargs), PromiseGetNamespace(fp->caller)) ? "!any" : "any"), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallNow(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];
    time_t cftime;

    buffer[0] = '\0';

/* begin fn specific content */

    cftime = CFSTARTTIME;

    snprintf(buffer, CF_BUFSIZE - 1, "%ld", (long) cftime);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallStrftime(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    /* begin fn specific content */

    char *mode = RlistScalarValue(finalargs);
    char *format_string = RlistScalarValue(finalargs->next);
    // this will be a problem on 32-bit systems...
    const time_t when = IntFromString(RlistScalarValue(finalargs->next->next));

    char buffer[CF_BUFSIZE];
    buffer[0] = '\0';

    struct tm* tm;

    if (0 == strcmp("gmtime", mode))
    {
        tm = gmtime(&when);
    }
    else
    {
        tm = localtime(&when);
    }

    if(tm != NULL)
    {
        strftime(buffer, sizeof buffer, format_string, tm);
    }
    else
    {
        Log(LOG_LEVEL_WARNING, "Function strftime, the given time stamp '%ld' was invalid. (strftime: %s)", when, GetErrorStr());
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallEval(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char *input = RlistScalarValue(finalargs);
    char *type = RlistScalarValue(finalargs->next);
    char *options = RlistScalarValue(finalargs->next->next);
    if (0 != strcmp(type, "math") || 0 != strcmp(options, "infix"))
    {
        Log(LOG_LEVEL_ERR, "Unknown %s evaluation type %s or options %s", fp->name, type, options);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    char failure[CF_BUFSIZE];
    memset(failure, 0, sizeof(failure));

    char out[CF_BUFSIZE];
    memset(out, 0, sizeof(out));

    double result = EvaluateMathInfix(ctx, input, failure);
    if (strlen(failure) > 0)
    {
        Log(LOG_LEVEL_INFO, "%s error: %s (input '%s')", fp->name, failure, input);
        //return (FnCallResult) { FNCALL_FAILURE };
        memset(out, 0, sizeof(out));
    }
    else
    {
        sprintf(out, "%lf", result);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(out), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/
/* Read functions                                                    */
/*********************************************************************/

static FnCallResult FnCallReadFile(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char *contents;

    char *filename = RlistScalarValue(finalargs);
    int maxsize = IntFromString(RlistScalarValue(finalargs->next));

    // Read once to validate structure of file in itemlist
    contents = CfReadFile(filename, maxsize);

    if (contents)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { contents, RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
}

/*********************************************************************/

static FnCallResult ReadList(EvalContext *ctx, FnCall *fp, Rlist *finalargs, DataType type)
{
    Rlist *rp, *newlist = NULL;
    char fnname[CF_MAXVARSIZE], *file_buffer = NULL;
    int noerrors = true, blanks = false;

    char *filename = RlistScalarValue(finalargs);
    char *comment = RlistScalarValue(finalargs->next);
    char *split = RlistScalarValue(finalargs->next->next);
    int maxent = IntFromString(RlistScalarValue(finalargs->next->next->next));
    int maxsize = IntFromString(RlistScalarValue(finalargs->next->next->next->next));

    // Read once to validate structure of file in itemlist
    snprintf(fnname, CF_MAXVARSIZE - 1, "read%slist", DataTypeToString(type));

    file_buffer = (char *) CfReadFile(filename, maxsize);

    if (file_buffer == NULL)
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
    else
    {
        file_buffer = StripPatterns(ctx, file_buffer, comment, filename);

        if (file_buffer == NULL)
        {
            return (FnCallResult) { FNCALL_SUCCESS, { NULL, RVAL_TYPE_LIST } };
        }
        else
        {
            newlist = RlistFromSplitRegex(ctx, file_buffer, split, maxent, blanks);
        }
    }

    switch (type)
    {
    case DATA_TYPE_STRING:
        break;

    case DATA_TYPE_INT:
        for (rp = newlist; rp != NULL; rp = rp->next)
        {
            if (IntFromString(RlistScalarValue(rp)) == CF_NOINT)
            {
                Log(LOG_LEVEL_ERR, "Presumed int value '%s' read from file '%s' has no recognizable value",
                      RlistScalarValue(rp), filename);
                noerrors = false;
            }
        }
        break;

    case DATA_TYPE_REAL:
        for (rp = newlist; rp != NULL; rp = rp->next)
        {
            double real_value = 0;
            if (!DoubleFromString(RlistScalarValue(rp), &real_value))
            {
                Log(LOG_LEVEL_ERR, "Presumed real value '%s' read from file '%s' has no recognizable value",
                      RlistScalarValue(rp), filename);
                noerrors = false;
            }
        }
        break;

    default:
        ProgrammingError("Unhandled type in switch: %d", type);
    }

    free(file_buffer);

    if (newlist && noerrors)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { newlist, RVAL_TYPE_LIST } };
    }
    else
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

}

static FnCallResult FnCallReadStringList(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ReadList(ctx, fp, args, DATA_TYPE_STRING);
}

static FnCallResult FnCallReadIntList(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ReadList(ctx, fp, args, DATA_TYPE_INT);
}

static FnCallResult FnCallReadRealList(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ReadList(ctx, fp, args, DATA_TYPE_REAL);
}

static FnCallResult FnCallReadJson(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    const char *input_path = RlistScalarValue(args);
    size_t size_max = IntFromString(RlistScalarValue(args->next));

    char *contents = NULL;
    if (FileReadMax(&contents, input_path, size_max) == -1)
    {
        Log(LOG_LEVEL_ERR, "Error reading JSON input file '%s'", input_path);
        return (FnCallResult) { FNCALL_FAILURE, };
    }
    JsonElement *json = NULL;
    const char *data = contents;
    if (JsonParse(&data, &json) != JSON_PARSE_OK)
    {
        Log(LOG_LEVEL_ERR, "Error parsing JSON file '%s'", input_path);
        free(contents);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    free(contents);

    return (FnCallResult) { FNCALL_SUCCESS, (Rval) { json, RVAL_TYPE_CONTAINER } };
}

static FnCallResult FnCallParseJson(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    const char *data = RlistScalarValue(args);
    JsonElement *json = NULL;
    if (JsonParse(&data, &json) != JSON_PARSE_OK)
    {
        Log(LOG_LEVEL_ERR, "Error parsing JSON expression '%s'", data);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    return (FnCallResult) { FNCALL_SUCCESS, (Rval) { json, RVAL_TYPE_CONTAINER } };
}

/*********************************************************************/

static FnCallResult FnCallStoreJson(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buf[CF_BUFSIZE];
    char *varname = RlistScalarValue(finalargs);
    VarRef *ref = VarRefParseFromBundle(varname, PromiseGetBundle(fp->caller));

    DataType type = DATA_TYPE_NONE;
    Rval rval;
    EvalContextVariableGet(ctx, ref, &rval, &type);

    if (type == DATA_TYPE_CONTAINER)
    {
        Writer *w = StringWriter();
        int length;

        JsonWrite(w, RvalContainerValue(rval), 0);
        Log(LOG_LEVEL_DEBUG, "%s: from data container %s, got JSON data '%s'", fp->name, varname, StringWriterData(w));

        length = strlen(StringWriterData(w));
        if (length >= CF_BUFSIZE)
        {
            Log(LOG_LEVEL_INFO, "%s: truncating data container %s JSON data from %d bytes to %d", fp->name, varname, length, CF_BUFSIZE);
        }

        snprintf(buf, CF_BUFSIZE, "%s", StringWriterData(w));
        WriterClose(w);
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "%s: data container %s could not be found or has an invalid type", fp->name, varname);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buf), RVAL_TYPE_SCALAR } };
}


/*********************************************************************/

static FnCallResult ReadArray(EvalContext *ctx, FnCall *fp, Rlist *finalargs, DataType type, int intIndex)
/* lval,filename,separator,comment,Max number of bytes  */
{
    char fnname[CF_MAXVARSIZE], *file_buffer = NULL;
    int entries = 0;

    /* Arg validation */

    if (intIndex)
    {
        snprintf(fnname, CF_MAXVARSIZE - 1, "read%sarrayidx", DataTypeToString(type));
    }
    else
    {
        snprintf(fnname, CF_MAXVARSIZE - 1, "read%sarray", DataTypeToString(type));
    }

/* begin fn specific content */

    /* 6 args: array_lval,filename,comment_regex,split_regex,max number of entries,maxfilesize  */

    char *array_lval = RlistScalarValue(finalargs);
    char *filename = RlistScalarValue(finalargs->next);
    char *comment = RlistScalarValue(finalargs->next->next);
    char *split = RlistScalarValue(finalargs->next->next->next);
    int maxent = IntFromString(RlistScalarValue(finalargs->next->next->next->next));
    int maxsize = IntFromString(RlistScalarValue(finalargs->next->next->next->next->next));

// Read once to validate structure of file in itemlist
    file_buffer = CfReadFile(filename, maxsize);
    if (!file_buffer)
    {
        entries = 0;
    }
    else
    {
        file_buffer = StripPatterns(ctx, file_buffer, comment, filename);

        if (file_buffer == NULL)
        {
            entries = 0;
        }
        else
        {
            entries = BuildLineArray(ctx, PromiseGetBundle(fp->caller), array_lval, file_buffer, split, maxent, type, intIndex);
        }
    }

    switch (type)
    {
    case DATA_TYPE_STRING:
    case DATA_TYPE_INT:
    case DATA_TYPE_REAL:
        break;

    default:
        ProgrammingError("Unhandled type in switch: %d", type);
    }

    free(file_buffer);

/* Return the number of lines in array */

    snprintf(fnname, CF_MAXVARSIZE - 1, "%d", entries);
    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(fnname), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallReadStringArray(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ReadArray(ctx, fp, args, DATA_TYPE_STRING, false);
}

/*********************************************************************/

static FnCallResult FnCallReadStringArrayIndex(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ReadArray(ctx, fp, args, DATA_TYPE_STRING, true);
}

/*********************************************************************/

static FnCallResult FnCallReadIntArray(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ReadArray(ctx, fp, args, DATA_TYPE_INT, false);
}

/*********************************************************************/

static FnCallResult FnCallReadRealArray(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ReadArray(ctx, fp, args, DATA_TYPE_REAL, false);
}

/*********************************************************************/

static FnCallResult ParseArray(EvalContext *ctx, FnCall *fp, Rlist *finalargs, DataType type, int intIndex)
/* lval,filename,separator,comment,Max number of bytes  */
{
    char fnname[CF_MAXVARSIZE];
    int entries = 0;

    /* Arg validation */

    if (intIndex)
    {
        snprintf(fnname, CF_MAXVARSIZE - 1, "read%sarrayidx", DataTypeToString(type));
    }
    else
    {
        snprintf(fnname, CF_MAXVARSIZE - 1, "read%sarray", DataTypeToString(type));
    }

/* begin fn specific content */

    /* 6 args: array_lval,instring,comment_regex,split_regex,max number of entries,maxfilesize  */

    char *array_lval = RlistScalarValue(finalargs);
    char *instring = xstrdup(RlistScalarValue(finalargs->next));
    char *comment = RlistScalarValue(finalargs->next->next);
    char *split = RlistScalarValue(finalargs->next->next->next);
    int maxent = IntFromString(RlistScalarValue(finalargs->next->next->next->next));
    int maxsize = IntFromString(RlistScalarValue(finalargs->next->next->next->next->next));

// Read once to validate structure of file in itemlist

    Log(LOG_LEVEL_DEBUG, "Parse string data from string '%s' - , maxent %d, maxsize %d", instring, maxent, maxsize);

    if (instring == NULL)
    {
        entries = 0;
    }
    else
    {
        instring = StripPatterns(ctx, instring, comment, "string argument 2");

        if (instring == NULL)
        {
            entries = 0;
        }
        else
        {
            entries = BuildLineArray(ctx, PromiseGetBundle(fp->caller), array_lval, instring, split, maxent, type, intIndex);
        }
    }

    switch (type)
    {
    case DATA_TYPE_STRING:
    case DATA_TYPE_INT:
    case DATA_TYPE_REAL:
        break;

    default:
        ProgrammingError("Unhandled type in switch: %d", type);
    }

    free(instring);

/* Return the number of lines in array */

    snprintf(fnname, CF_MAXVARSIZE - 1, "%d", entries);
    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(fnname), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

static FnCallResult FnCallParseStringArray(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ParseArray(ctx, fp, args, DATA_TYPE_STRING, false);
}

/*********************************************************************/

static FnCallResult FnCallParseStringArrayIndex(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ParseArray(ctx, fp, args, DATA_TYPE_STRING, true);
}

/*********************************************************************/

static FnCallResult FnCallParseIntArray(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ParseArray(ctx, fp, args, DATA_TYPE_INT, false);
}

/*********************************************************************/

static FnCallResult FnCallParseRealArray(EvalContext *ctx, FnCall *fp, Rlist *args)
{
    return ParseArray(ctx, fp, args, DATA_TYPE_REAL, false);
}

/*********************************************************************/

static FnCallResult FnCallSplitString(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *newlist = NULL;

/* begin fn specific content */

    /* 2args: string,split_regex,max  */

    char *string = RlistScalarValue(finalargs);
    char *split = RlistScalarValue(finalargs->next);
    int max = IntFromString(RlistScalarValue(finalargs->next->next));

// Read once to validate structure of file in itemlist

    newlist = RlistFromSplitRegex(ctx, string, split, max, true);

    if (newlist == NULL)
    {
        RlistPrepend(&newlist, CF_NULL_VALUE, RVAL_TYPE_SCALAR);
    }

    return (FnCallResult) { FNCALL_SUCCESS, { newlist, RVAL_TYPE_LIST } };
}

/*********************************************************************/

static FnCallResult FnCallFileSexist(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    Rlist *rp, *files;
    char buffer[CF_BUFSIZE], naked[CF_MAXVARSIZE];
    Rval retval;
    struct stat sb;

    buffer[0] = '\0';

/* begin fn specific content */

    char *listvar = RlistScalarValue(finalargs);

    if (IsVarList(listvar))
    {
        GetNaked(naked, listvar);
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Function filesexist was promised a list called '%s' but this was not found", listvar);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    VarRef *ref = VarRefParse(naked);

    if (!EvalContextVariableGet(ctx, ref, &retval, NULL))
    {
        Log(LOG_LEVEL_VERBOSE, "Function filesexist was promised a list called '%s' but this was not found", listvar);
        VarRefDestroy(ref);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    VarRefDestroy(ref);

    if (retval.type != RVAL_TYPE_LIST)
    {
        Log(LOG_LEVEL_VERBOSE, "Function filesexist was promised a list called '%s' but this variable is not a list",
              listvar);
        return (FnCallResult) { FNCALL_FAILURE };
    }

    files = (Rlist *) retval.item;

    strcpy(buffer, "any");

    for (rp = files; rp != NULL; rp = rp->next)
    {
        if (stat(RlistScalarValue(rp), &sb) == -1)
        {
            strcpy(buffer, "!any");
            break;
        }
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/
/* LDAP Nova features                                                */
/*********************************************************************/

static FnCallResult FnCallLDAPValue(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE], handle[CF_BUFSIZE];
    void *newval = NULL;

/* begin fn specific content */

    char *uri = RlistScalarValue(finalargs);
    char *dn = RlistScalarValue(finalargs->next);
    char *filter = RlistScalarValue(finalargs->next->next);
    char *name = RlistScalarValue(finalargs->next->next->next);
    char *scope = RlistScalarValue(finalargs->next->next->next->next);
    char *sec = RlistScalarValue(finalargs->next->next->next->next->next);

    snprintf(handle, CF_BUFSIZE, "%s_%s_%s_%s", dn, filter, name, scope);

    if ((newval = CfLDAPValue(uri, dn, filter, name, scope, sec)))
    {
        CacheUnreliableValue("ldapvalue", handle, newval);
    }
    else
    {
        if (RetrieveUnreliableValue("ldapvalue", handle, buffer))
        {
            newval = xstrdup(buffer);
        }
    }

    if (newval)
    {
        return (FnCallResult) { FNCALL_SUCCESS, { newval, RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }
}

/*********************************************************************/

static FnCallResult FnCallLDAPArray(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    void *newval;

/* begin fn specific content */

    char *array = RlistScalarValue(finalargs);
    char *uri = RlistScalarValue(finalargs->next);
    char *dn = RlistScalarValue(finalargs->next->next);
    char *filter = RlistScalarValue(finalargs->next->next->next);
    char *scope = RlistScalarValue(finalargs->next->next->next->next);
    char *sec = RlistScalarValue(finalargs->next->next->next->next->next);

    if ((newval = CfLDAPArray(ctx, PromiseGetBundle(fp->caller), array, uri, dn, filter, scope, sec)))
    {
        return (FnCallResult) { FNCALL_SUCCESS, { newval, RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

}

/*********************************************************************/

static FnCallResult FnCallLDAPList(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    void *newval;

/* begin fn specific content */

    char *uri = RlistScalarValue(finalargs);
    char *dn = RlistScalarValue(finalargs->next);
    char *filter = RlistScalarValue(finalargs->next->next);
    char *name = RlistScalarValue(finalargs->next->next->next);
    char *scope = RlistScalarValue(finalargs->next->next->next->next);
    char *sec = RlistScalarValue(finalargs->next->next->next->next->next);

    if ((newval = CfLDAPList(uri, dn, filter, name, scope, sec)))
    {
        return (FnCallResult) { FNCALL_SUCCESS, { newval, RVAL_TYPE_LIST } };
    }
    else
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

}

/*********************************************************************/

static FnCallResult FnCallRegLDAP(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    void *newval;

/* begin fn specific content */

    char *uri = RlistScalarValue(finalargs);
    char *dn = RlistScalarValue(finalargs->next);
    char *filter = RlistScalarValue(finalargs->next->next);
    char *name = RlistScalarValue(finalargs->next->next->next);
    char *scope = RlistScalarValue(finalargs->next->next->next->next);
    char *regex = RlistScalarValue(finalargs->next->next->next->next->next);
    char *sec = RlistScalarValue(finalargs->next->next->next->next->next->next);

    if ((newval = CfRegLDAP(ctx, uri, dn, filter, name, scope, regex, sec)))
    {
        return (FnCallResult) { FNCALL_SUCCESS, { newval, RVAL_TYPE_SCALAR } };
    }
    else
    {
        return (FnCallResult) { FNCALL_FAILURE };
    }

}

/*********************************************************************/

#define KILOBYTE 1024

static FnCallResult FnCallDiskFree(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];
    off_t df;

    buffer[0] = '\0';

    df = GetDiskUsage(RlistScalarValue(finalargs), cfabs);

    if (df == CF_INFINITY)
    {
        df = 0;
    }

    /* Result is in kilobytes */
    snprintf(buffer, CF_BUFSIZE - 1, "%jd", ((intmax_t) df) / KILOBYTE);

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

#if !defined(__MINGW32__)

FnCallResult FnCallUserExists(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];
    struct passwd *pw;
    uid_t uid = CF_SAME_OWNER;
    char *arg = RlistScalarValue(finalargs);

    buffer[0] = '\0';

/* begin fn specific content */

    strcpy(buffer, CF_ANYCLASS);

    if (StringIsNumeric(arg))
    {
        uid = Str2Uid(arg, NULL, NULL);

        if (uid == CF_SAME_OWNER || uid == CF_UNKNOWN_OWNER)
        {
            return (FnCallResult){ FNCALL_FAILURE };
        }

        if ((pw = getpwuid(uid)) == NULL)
        {
            strcpy(buffer, "!any");
        }
    }
    else if ((pw = getpwnam(arg)) == NULL)
    {
        strcpy(buffer, "!any");
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

/*********************************************************************/

FnCallResult FnCallGroupExists(EvalContext *ctx, FnCall *fp, Rlist *finalargs)
{
    char buffer[CF_BUFSIZE];
    struct group *gr;
    gid_t gid = CF_SAME_GROUP;
    char *arg = RlistScalarValue(finalargs);

    buffer[0] = '\0';

/* begin fn specific content */

    strcpy(buffer, CF_ANYCLASS);

    if (isdigit((int) *arg))
    {
        gid = Str2Gid(arg, NULL, NULL);

        if (gid == CF_SAME_GROUP || gid == CF_UNKNOWN_GROUP)
        {
            return (FnCallResult) { FNCALL_FAILURE };
        }

        if ((gr = getgrgid(gid)) == NULL)
        {
            strcpy(buffer, "!any");
        }
    }
    else if ((gr = getgrnam(arg)) == NULL)
    {
        strcpy(buffer, "!any");
    }

    return (FnCallResult) { FNCALL_SUCCESS, { xstrdup(buffer), RVAL_TYPE_SCALAR } };
}

#endif /* !defined(__MINGW32__) */

/*********************************************************************/
/* Level                                                             */
/*********************************************************************/

static void *CfReadFile(char *filename, int maxsize)
{
    struct stat sb;
    char *result = NULL;
    FILE *fp;
    size_t size, bytes_read;
    int i, newlines = 0;
    size_t buflen = 0;

    /* Because of strings hard-coded limits read up to CF_BUFSIZE bytes */

    if ((fp = fopen(filename, "r")) == NULL)
    {
        Log(LOG_LEVEL_INFO, "readfile: Could not open file '%s' (fopen: %s)", filename, GetErrorStr());
        return NULL;
    }

    if (stat(filename, &sb) == -1)
    {
        if (THIS_AGENT_TYPE == AGENT_TYPE_COMMON)
        {
            Log(LOG_LEVEL_INFO, "readfile: Could not examine file '%s'", filename);
        }
        else
        {
            if (IsCf3VarString(filename))
            {
                Log(LOG_LEVEL_VERBOSE, "readfile: Cannot converge/reduce variable '%s' yet .. assuming it will resolve later",
                      filename);
            }
            else
            {
                Log(LOG_LEVEL_INFO, "readfile: Could not examine file '%s' (stat: %s)",
                      filename, GetErrorStr());
            }
        }
        clearerr(fp);
        fclose(fp);
        return NULL;
    }

    // If requested, force read because of broken /proc|/sys files semantics.
    if (maxsize == 0)
    {
        buflen = CF_BUFSIZE;
    }
    else
    {
        buflen = MIN(CF_BUFSIZE, maxsize);
    }

    if (sb.st_size > maxsize && maxsize != 0)
    {
        buflen = maxsize;
        Log(LOG_LEVEL_INFO, "readfile: Truncating file '%s' to %d bytes, as requested by the maxsize parameter",
              filename, maxsize);
    }

    if (sb.st_size > CF_BUFSIZE)
    {
        buflen = CF_BUFSIZE;
        Log(LOG_LEVEL_INFO, "readfile: Truncating file '%s' to %d bytes, because of internal limits",
              filename, CF_BUFSIZE);
    }

    result = xcalloc(1, buflen+1); // Extra space for '\0'
    bytes_read = fread(result, 1, buflen, fp);

    if (ferror(fp))
    {
        Log(LOG_LEVEL_INFO, "readfile: Error while reading file '%s' (fread: %s)",
              filename, GetErrorStr());
        clearerr(fp);
        free(result);
        return NULL;
    }

    if (bytes_read < buflen)
    {
        buflen = bytes_read;
        result = xrealloc(result, buflen+1);
    }

    result[buflen] = '\0';

    size = buflen;

    if ( size > 0)
    {
      for (i = 0; i < size - 1 && result[i] != '\0' ; i++)
      {
          if (result[i] == '\n' || result[i] == '\r')
          {
              newlines++;
          }
      }

      if (newlines == 0 && (result[size - 1] == '\n' || result[size - 1] == '\r'))
      {
          result[size - 1] = '\0';
      }
    }

    clearerr(fp);
    fclose(fp);
    return (void *) result;
}

/*********************************************************************/

static char *StripPatterns(EvalContext *ctx, char *file_buffer, char *pattern, char *filename)
{
    int start, end;
    int count = 0;

    if (!NULL_OR_EMPTY(pattern))
    {
        while (BlockTextMatch(ctx, pattern, file_buffer, &start, &end))
        {
            CloseStringHole(file_buffer, start, end);

            if (count++ > strlen(file_buffer))
            {
                Log(LOG_LEVEL_ERR,
                    "Comment regex '%s' was irreconcilable reading input '%s' probably because it legally matches nothing",
                      pattern, filename);
                return file_buffer;
            }
        }
    }

    return file_buffer;
}

/*********************************************************************/

static void CloseStringHole(char *s, int start, int end)
{
    int off = end - start;
    char *sp;

    if (off <= 0)
    {
        return;
    }

    for (sp = s + start; *(sp + off) != '\0'; sp++)
    {
        *sp = *(sp + off);
    }

    *sp = '\0';
}

/*********************************************************************/

static int BuildLineArray(EvalContext *ctx, const Bundle *bundle, char *array_lval, char *file_buffer, char *split, int maxent, DataType type,
                          int intIndex)
{
    char *sp, linebuf[CF_BUFSIZE], name[CF_MAXVARSIZE], first_one[CF_MAXVARSIZE];
    Rlist *rp, *newlist = NULL;
    int allowblanks = true, vcount, hcount, lcount = 0;
    int lineLen;

    memset(linebuf, 0, CF_BUFSIZE);
    hcount = 0;

    for (sp = file_buffer; hcount < maxent && *sp != '\0'; sp++)
    {
        linebuf[0] = '\0';
        sscanf(sp, "%1023[^\n]", linebuf);

        lineLen = strlen(linebuf);

        if (lineLen == 0)
        {
            continue;
        }
        else if (lineLen == 1 && linebuf[0] == '\r')
        {
            continue;
        }

        if (linebuf[lineLen - 1] == '\r')
        {
            linebuf[lineLen - 1] = '\0';
        }

        if (lcount++ > CF_HASHTABLESIZE)
        {
            Log(LOG_LEVEL_ERR, "Array is too big to be read into CFEngine (max 4000)");
            break;
        }

        newlist = RlistFromSplitRegex(ctx, linebuf, split, maxent, allowblanks);

        vcount = 0;
        first_one[0] = '\0';

        for (rp = newlist; rp != NULL; rp = rp->next)
        {
            char this_rval[CF_MAXVARSIZE];
            long ival;

            switch (type)
            {
            case DATA_TYPE_STRING:
                strncpy(this_rval, RlistScalarValue(rp), CF_MAXVARSIZE - 1);
                break;

            case DATA_TYPE_INT:
                ival = IntFromString(RlistScalarValue(rp));
                snprintf(this_rval, CF_MAXVARSIZE, "%d", (int) ival);
                break;

            case DATA_TYPE_REAL:
                {
                    double real_value = 0;
                    if (!DoubleFromString(RlistScalarValue(rp), &real_value))
                    {
                        FatalError(ctx, "Could not convert rval to double");
                    }
                }
                sscanf(RlistScalarValue(rp), "%255s", this_rval);
                break;

            default:
                ProgrammingError("Unhandled type in switch: %d", type);
            }

            if (strlen(first_one) == 0)
            {
                strncpy(first_one, this_rval, CF_MAXVARSIZE - 1);
            }

            if (intIndex)
            {
                snprintf(name, CF_MAXVARSIZE, "%s[%d][%d]", array_lval, hcount, vcount);
            }
            else
            {
                snprintf(name, CF_MAXVARSIZE, "%s[%s][%d]", array_lval, first_one, vcount);
            }

            VarRef *ref = VarRefParseFromBundle(name, bundle);
            EvalContextVariablePut(ctx, ref, this_rval, type);
            VarRefDestroy(ref);
            vcount++;
        }

        RlistDestroy(newlist);

        hcount++;
        sp += lineLen;

        if (*sp == '\0')        // either \n or \0
        {
            break;
        }
    }

/* Don't free data - goes into vars */

    return hcount;
}

/*********************************************************************/

static int ExecModule(EvalContext *ctx, char *command, const char *ns)
{
    FILE *pp;
    char *sp, line[CF_BUFSIZE];
    char context[CF_BUFSIZE];
    int print = false;

    context[0] = '\0';

    if ((pp = cf_popen(command, "rt", true)) == NULL)
    {
        Log(LOG_LEVEL_ERR, "Couldn't open pipe from '%s'. (cf_popen: %s)", command, GetErrorStr());
        return false;
    }

    for (;;)
    {
        ssize_t res = CfReadLine(line, CF_BUFSIZE, pp);

        if (res == 0)
        {
            break;
        }

        if (res == -1)
        {
            Log(LOG_LEVEL_ERR, "Unable to read output from '%s'. (fread: %s)", command, GetErrorStr());
            cf_pclose(pp);
            return false;
        }

        if (strlen(line) > CF_BUFSIZE - 80)
        {
            Log(LOG_LEVEL_ERR, "Line from module '%s' is too long to be sensible", command);
            break;
        }

        print = false;

        for (sp = line; *sp != '\0'; sp++)
        {
            if (!isspace((int) *sp))
            {
                print = true;
                break;
            }
        }

        ModuleProtocol(ctx, command, line, print, ns, context);
    }

    cf_pclose(pp);
    return true;
}

/*********************************************************************/
/* Level                                                             */
/*********************************************************************/

void ModuleProtocol(EvalContext *ctx, char *command, char *line, int print, const char *ns, char* context)
{
    char name[CF_BUFSIZE], content[CF_BUFSIZE];
    char arg0[CF_BUFSIZE];
    char *filename;

    if (*context == '\0')
    {
/* Infer namespace from script name */

        snprintf(arg0, CF_BUFSIZE, "%s", CommandArg0(command));
        filename = basename(arg0);

/* Canonicalize filename into acceptable namespace name*/

        CanonifyNameInPlace(filename);
        strcpy(context, filename);
        Log(LOG_LEVEL_VERBOSE, "Module context '%s'", context);
    }

    name[0] = '\0';
    content[0] = '\0';

    switch (*line)
    {
    case '^':
        content[0] = '\0';

        // Allow modules to set their variable context (up to 50 characters)
        if (1 == sscanf(line + 1, "context=%50[a-z]", content) && strlen(content) > 0)
        {
            Log(LOG_LEVEL_VERBOSE, "Module changed variable context from '%s' to '%s'", context, content);
            strcpy(context, content);
        }
        break;

    case '+':
        Log(LOG_LEVEL_VERBOSE, "Activated classes '%s'", line + 1);
        if (CheckID(line + 1))
        {
             EvalContextClassPut(ctx, ns, line + 1, true, CONTEXT_SCOPE_NAMESPACE);
        }
        break;
    case '-':
        Log(LOG_LEVEL_VERBOSE, "Deactivated classes '%s'", line + 1);
        if (CheckID(line + 1))
        {
            if (line[1] != '\0')
            {
                StringSet *negated = StringSetFromString(line + 1, ',');
                StringSetIterator it = StringSetIteratorInit(negated);
                const char *negated_context = NULL;
                while ((negated_context = StringSetIteratorNext(&it)))
                {
                    Class *cls = EvalContextClassGet(ctx, NULL, negated_context);
                    if (cls && !cls->is_soft)
                    {
                        FatalError(ctx, "Cannot negate the reserved class '%s'", negated_context);
                    }

                    ClassRef ref = ClassRefParse(negated_context);
                    EvalContextClassRemove(ctx, ref.ns, ref.name);
                    ClassRefDestroy(ref);
                }
                StringSetDestroy(negated);
            }
        }
        break;
    case '=':
        content[0] = '\0';
        sscanf(line + 1, "%[^=]=%[^\n]", name, content);

        if (CheckID(name))
        {
            Log(LOG_LEVEL_VERBOSE, "Defined variable '%s' in context '%s' with value '%s'", name, context, content);
            VarRef *ref = VarRefParseFromScope(name, context);
            EvalContextVariablePut(ctx, ref, content, DATA_TYPE_STRING);
            VarRefDestroy(ref);
        }
        break;

    case '@':
        content[0] = '\0';
        sscanf(line + 1, "%[^=]=%[^\n]", name, content);

        if (CheckID(name))
        {
            Rlist *list = NULL;

            list = RlistParseString(content);
            Log(LOG_LEVEL_VERBOSE, "Defined variable '%s' in context '%s' with value '%s'", name, context, content);

            VarRef *ref = VarRefParseFromScope(name, context);
            EvalContextVariablePut(ctx, ref, list, DATA_TYPE_STRING_LIST);
            VarRefDestroy(ref);
        }
        break;

    case '\0':
        break;

    default:
        if (print)
        {
            Log(LOG_LEVEL_INFO, "M '%s': %s", command, line);
        }
        break;
    }
}

/*********************************************************************/
/* Level                                                             */
/*********************************************************************/

static int CheckID(char *id)
{
    char *sp;

    for (sp = id; *sp != '\0'; sp++)
    {
        if (!isalnum((int) *sp) && (*sp != '.') && (*sp != '-') && (*sp != '_') && (*sp != '[') && (*sp != ']'))
        {
            Log(LOG_LEVEL_ERR,
                  "Module protocol contained an illegal character '%c' in class/variable identifier '%s'.", *sp,
                  id);
            return false;
        }
    }

    return true;
}

/*********************************************************************/

FnCallResult CallFunction(EvalContext *ctx, const FnCallType *function, FnCall *fp, Rlist *expargs)
{
    ArgTemplate(ctx, fp, function->args, expargs);
    return (*function->impl) (ctx, fp, expargs);
}

/*********************************************************/
/* Function prototypes                                   */
/*********************************************************/

FnCallArg ACCESSEDBEFORE_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Newer filename"},
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Older filename"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg ACCUM_ARGS[] =
{
    {"0,1000", DATA_TYPE_INT, "Years"},
    {"0,1000", DATA_TYPE_INT, "Months"},
    {"0,1000", DATA_TYPE_INT, "Days"},
    {"0,1000", DATA_TYPE_INT, "Hours"},
    {"0,1000", DATA_TYPE_INT, "Minutes"},
    {"0,40000", DATA_TYPE_INT, "Seconds"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg AND_ARGS[] =
{
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg AGO_ARGS[] =
{
    {"0,1000", DATA_TYPE_INT, "Years"},
    {"0,1000", DATA_TYPE_INT, "Months"},
    {"0,1000", DATA_TYPE_INT, "Days"},
    {"0,1000", DATA_TYPE_INT, "Hours"},
    {"0,1000", DATA_TYPE_INT, "Minutes"},
    {"0,40000", DATA_TYPE_INT, "Seconds"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg LATERTHAN_ARGS[] =
{
    {"0,1000", DATA_TYPE_INT, "Years"},
    {"0,1000", DATA_TYPE_INT, "Months"},
    {"0,1000", DATA_TYPE_INT, "Days"},
    {"0,1000", DATA_TYPE_INT, "Hours"},
    {"0,1000", DATA_TYPE_INT, "Minutes"},
    {"0,40000", DATA_TYPE_INT, "Seconds"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg CANONIFY_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "String containing non-identifier characters"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg CHANGEDBEFORE_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Newer filename"},
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Older filename"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg CLASSIFY_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Input string"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg CLASSMATCH_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg CONCAT_ARGS[] =
{
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg COUNTCLASSESMATCHING_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg COUNTLINESMATCHING_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Filename"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg DIRNAME_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "File path"},
    {NULL, DATA_TYPE_NONE, NULL},
};

FnCallArg DISKFREE_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "File system directory"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg ESCAPE_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "IP address or string to escape"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg EXECRESULT_ARGS[] =
{
    {CF_PATHRANGE, DATA_TYPE_STRING, "Fully qualified command path"},
    {"useshell,noshell,powershell", DATA_TYPE_OPTION, "Shell encapsulation option"},
    {NULL, DATA_TYPE_NONE, NULL}
};

// fileexists, isdir,isplain,islink

FnCallArg FILESTAT_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "File object name"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg FILESTAT_DETAIL_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "File object name"},
    {"size,gid,uid,ino,nlink,ctime,atime,mtime,mode,modeoct,permstr,permoct,type,devno,dev_minor,dev_major,basename,dirname,linktarget,linktarget_shallow", DATA_TYPE_OPTION, "stat() field to get"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg FILESEXIST_ARGS[] =
{
    {CF_NAKEDLRANGE, DATA_TYPE_STRING, "Array identifier containing list"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg FINDFILES_ARGS[] =
{
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg FILTER_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression or string"},
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {CF_BOOL, DATA_TYPE_OPTION, "Match as regular expression if true, as exact string otherwise"},
    {CF_BOOL, DATA_TYPE_OPTION, "Invert matches"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of matches to return"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg GETFIELDS_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression to match line"},
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Filename to read"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression to split fields"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Return array name"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg GETINDICES_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine array identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg GETUSERS_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Comma separated list of User names"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Comma separated list of UserID numbers"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg GETENV_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "Name of environment variable"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of characters to read "},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg GETGID_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Group name in text"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg GETUID_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "User name in text"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg GREP_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg GROUPEXISTS_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Group name or identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg HASH_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Input text"},
    {"md5,sha1,sha256,sha512,sha384,crypt", DATA_TYPE_OPTION, "Hash or digest algorithm"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg HASHMATCH_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Filename to hash"},
    {"md5,sha1,crypt,cf_sha224,cf_sha256,cf_sha384,cf_sha512", DATA_TYPE_OPTION, "Hash or digest algorithm"},
    {CF_IDRANGE, DATA_TYPE_STRING, "ASCII representation of hash for comparison"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg HOST2IP_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Host name in ascii"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg IP2HOST_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "IP address (IPv4 or IPv6)"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg HOSTINNETGROUP_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Netgroup name"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg HOSTRANGE_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Hostname prefix"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Enumerated range"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg HOSTSSEEN_ARGS[] =
{
    {CF_VALRANGE, DATA_TYPE_INT, "Horizon since last seen in hours"},
    {"lastseen,notseen", DATA_TYPE_OPTION, "Complements for selection policy"},
    {"name,address", DATA_TYPE_OPTION, "Type of return value desired"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg HOSTSWITHCLASS_ARGS[] =
{
    {"[a-zA-Z0-9_]+", DATA_TYPE_STRING, "Class name to look for"},
    {"name,address", DATA_TYPE_OPTION, "Type of return value desired"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg IFELSE_ARGS[] =
{
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg IPRANGE_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "IP address range syntax"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg IRANGE_ARGS[] =
{
    {CF_INTRANGE, DATA_TYPE_INT, "Integer"},
    {CF_INTRANGE, DATA_TYPE_INT, "Integer"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg ISGREATERTHAN_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Larger string or value"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Smaller string or value"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg ISLESSTHAN_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Smaller string or value"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Larger string or value"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg ISNEWERTHAN_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Newer file name"},
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Older file name"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg ISVARIABLE_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "Variable identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg JOIN_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Join glue-string"},
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg LASTNODE_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Input string"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Link separator, e.g. /,:"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg LDAPARRAY_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Array name"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "URI"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Distinguished name"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Filter"},
    {"subtree,onelevel,base", DATA_TYPE_OPTION, "Search scope policy"},
    {"none,ssl,sasl", DATA_TYPE_OPTION, "Security level"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg LDAPLIST_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "URI"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Distinguished name"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Filter"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Record name"},
    {"subtree,onelevel,base", DATA_TYPE_OPTION, "Search scope policy"},
    {"none,ssl,sasl", DATA_TYPE_OPTION, "Security level"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg LDAPVALUE_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "URI"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Distinguished name"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Filter"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Record name"},
    {"subtree,onelevel,base", DATA_TYPE_OPTION, "Search scope policy"},
    {"none,ssl,sasl", DATA_TYPE_OPTION, "Security level"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg LSDIRLIST_ARGS[] =
{
    {CF_PATHRANGE, DATA_TYPE_STRING, "Path to base directory"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression to match files or blank"},
    {CF_BOOL, DATA_TYPE_OPTION, "Include the base path in the list"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg MAPLIST_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Pattern based on $(this) as original text"},
    {CF_IDRANGE, DATA_TYPE_STRING, "The name of the list variable to map"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg MAPARRAY_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Pattern based on $(this.k) and $(this.v) as original text"},
    {CF_IDRANGE, DATA_TYPE_STRING, "The name of the array variable to map"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg MERGEDATA_ARGS[] =
{
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg NOT_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Class value"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg NOW_ARGS[] =
{
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg OR_ARGS[] =
{
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg SUM_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "A list of arbitrary real values"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg PRODUCT_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "A list of arbitrary real values"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg DATE_ARGS[] =
{
    {"1970,3000", DATA_TYPE_INT, "Year"},
    {"1,12", DATA_TYPE_INT, "Month"},
    {"1,31", DATA_TYPE_INT, "Day"},
    {"0,23", DATA_TYPE_INT, "Hour"},
    {"0,59", DATA_TYPE_INT, "Minute"},
    {"0,59", DATA_TYPE_INT, "Second"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg PEERS_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "File name of host list"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Comment regex pattern"},
    {CF_VALRANGE, DATA_TYPE_INT, "Peer group size"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg PEERLEADER_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "File name of host list"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Comment regex pattern"},
    {CF_VALRANGE, DATA_TYPE_INT, "Peer group size"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg PEERLEADERS_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "File name of host list"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Comment regex pattern"},
    {CF_VALRANGE, DATA_TYPE_INT, "Peer group size"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg RANDOMINT_ARGS[] =
{
    {CF_INTRANGE, DATA_TYPE_INT, "Lower inclusive bound"},
    {CF_INTRANGE, DATA_TYPE_INT, "Upper inclusive bound"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg READFILE_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "File name"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of bytes to read"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg READSTRINGARRAY_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "Array identifier to populate"},
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "File name to read"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex matching comments"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex to split data"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of entries to read"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum bytes to read"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg PARSESTRINGARRAY_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "Array identifier to populate"},
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "A string to parse for input data"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex matching comments"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex to split data"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of entries to read"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum bytes to read"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg READSTRINGARRAYIDX_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "Array identifier to populate"},
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "A string to parse for input data"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex matching comments"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex to split data"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of entries to read"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum bytes to read"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg PARSESTRINGARRAYIDX_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "Array identifier to populate"},
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "A string to parse for input data"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex matching comments"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex to split data"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of entries to read"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum bytes to read"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg READSTRINGLIST_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "File name to read"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex matching comments"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex to split data"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of entries to read"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum bytes to read"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg READJSON_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "File name to read"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of bytes to read"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg PARSEJSON_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "JSON string to parse"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg STOREJSON_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine data container identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg READTCP_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Host name or IP address of server socket"},
    {CF_VALRANGE, DATA_TYPE_INT, "Port number"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Protocol query string"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of bytes to read"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg REGARRAY_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine array identifier"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg REGCMP_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Match string"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg REGEXTRACT_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Match string"},
    {CF_IDRANGE, DATA_TYPE_STRING, "Identifier for back-references"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg REGISTRYVALUE_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Windows registry key"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Windows registry value-id"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg REGLINE_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Filename to search"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg REGLIST_ARGS[] =
{
    {CF_NAKEDLRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg REGLDAP_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "URI"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Distinguished name"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Filter"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Record name"},
    {"subtree,onelevel,base", DATA_TYPE_OPTION, "Search scope policy"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex to match results"},
    {"none,ssl,sasl", DATA_TYPE_OPTION, "Security level"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg REMOTESCALAR_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "Variable identifier"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Hostname or IP address of server"},
    {CF_BOOL, DATA_TYPE_OPTION, "Use enryption"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg HUB_KNOWLEDGE_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "Variable identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg REMOTECLASSESMATCHING_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Server name or address"},
    {CF_BOOL, DATA_TYPE_OPTION, "Use encryption"},
    {CF_IDRANGE, DATA_TYPE_STRING, "Return class prefix"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg RETURNSZERO_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Fully qualified command path"},
    {"useshell,noshell,powershell", DATA_TYPE_OPTION, "Shell encapsulation option"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg RRANGE_ARGS[] =
{
    {CF_REALRANGE, DATA_TYPE_REAL, "Real number"},
    {CF_REALRANGE, DATA_TYPE_REAL, "Real number"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg SELECTSERVERS_ARGS[] =
{
    {CF_NAKEDLRANGE, DATA_TYPE_STRING, "The identifier of a cfengine list of hosts or addresses to contact"},
    {CF_VALRANGE, DATA_TYPE_INT, "The port number"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "A query string"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "A regular expression to match success"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of bytes to read from server"},
    {CF_IDRANGE, DATA_TYPE_STRING, "Name for array of results"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg SPLAYCLASS_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Input string for classification"},
    {"daily,hourly", DATA_TYPE_OPTION, "Splay time policy"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg SPLITSTRING_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "A data string"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regex to split on"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of pieces"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg STRCMP_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "String"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "String"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg STRFTIME_ARGS[] =
{
    {"gmtime,localtime", DATA_TYPE_OPTION, "Use GMT or local time"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "A format string"},
    {CF_VALRANGE, DATA_TYPE_INT, "The time as a Unix epoch offset"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg SUBLIST_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {"head,tail", DATA_TYPE_OPTION, "Whether to return elements from the head or from the tail of the list"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of elements to return"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg TRANSLATEPATH_ARGS[] =
{
    {CF_ABSPATHRANGE, DATA_TYPE_STRING, "Unix style path"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg USEMODULE_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Name of module command"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Argument string for the module"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg UNIQUE_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg NTH_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {CF_VALRANGE, DATA_TYPE_INT, "Offset of element to return"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg EVERY_SOME_NONE_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression or string"},
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg USEREXISTS_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "User name or identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg SORT_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {"lex,int,real,IP,ip,MAC,mac", DATA_TYPE_OPTION, "Sorting method: lex or int or real (floating point) or IPv4/IPv6 or MAC address"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg REVERSE_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg SHUFFLE_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {CF_ANYSTRING, DATA_TYPE_STRING, "Any seed string"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg LENGTH_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine list identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg SETOP_ARGS[] =
{
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine base list identifier"},
    {CF_IDRANGE, DATA_TYPE_STRING, "CFEngine filter list identifier"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg FORMAT_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "CFEngine format string"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg EVAL_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Input string"},
    {"math", DATA_TYPE_OPTION, "Evaluation type"},
    {"infix", DATA_TYPE_OPTION, "Evaluation options"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg BUNDLESMATCHING_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Regular expression"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg XFORM_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Input string"},
    {NULL, DATA_TYPE_NONE, NULL}
};

FnCallArg XFORM_SUBSTR_ARGS[] =
{
    {CF_ANYSTRING, DATA_TYPE_STRING, "Input string"},
    {CF_VALRANGE, DATA_TYPE_INT, "Maximum number of characters to return"},
    {NULL, DATA_TYPE_NONE, NULL}
};

/*********************************************************/
/* FnCalls are rvalues in certain promise constraints    */
/*********************************************************/

/* see cf3.defs.h enum fncalltype */

const FnCallType CF_FNCALL_TYPES[] =
{
    FnCallTypeNew("accessedbefore", DATA_TYPE_CONTEXT, ACCESSEDBEFORE_ARGS, &FnCallIsAccessedBefore, "True if arg1 was accessed before arg2 (atime)", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("accumulated", DATA_TYPE_INT, ACCUM_ARGS, &FnCallAccumulatedDate, "Convert an accumulated amount of time into a system representation", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("ago", DATA_TYPE_INT, AGO_ARGS, &FnCallAgoDate, "Convert a time relative to now to an integer system representation", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("and", DATA_TYPE_STRING, AND_ARGS, &FnCallAnd, "Calculate whether all arguments evaluate to true", true, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("bundlesmatching", DATA_TYPE_STRING_LIST, BUNDLESMATCHING_ARGS, &FnCallBundlesmatching, "Find all the bundles that match a regular expression", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("canonify", DATA_TYPE_STRING, CANONIFY_ARGS, &FnCallCanonify, "Convert an abitrary string into a legal class name", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("canonifyuniquely", DATA_TYPE_STRING, CANONIFY_ARGS, &FnCallCanonify, "Convert an abitrary string into a unique legal class name", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("concat", DATA_TYPE_STRING, CONCAT_ARGS, &FnCallConcat, "Concatenate all arguments into string", true, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("changedbefore", DATA_TYPE_CONTEXT, CHANGEDBEFORE_ARGS, &FnCallIsChangedBefore, "True if arg1 was changed before arg2 (ctime)", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("classify", DATA_TYPE_CONTEXT, CLASSIFY_ARGS, &FnCallClassify, "True if the canonicalization of the argument is a currently defined class", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("classmatch", DATA_TYPE_CONTEXT, CLASSMATCH_ARGS, &FnCallClassMatch, "True if the regular expression matches any currently defined class", false, FNCALL_CATEGORY_UTILS, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("classesmatching", DATA_TYPE_STRING_LIST, CLASSMATCH_ARGS, &FnCallClassesMatching, "List the defined classes matching regex arg1 and tag regexes arg2,arg3,...", true, FNCALL_CATEGORY_UTILS, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("countclassesmatching", DATA_TYPE_INT, COUNTCLASSESMATCHING_ARGS, &FnCallCountClassesMatching, "Count the number of defined classes matching regex arg1", false, FNCALL_CATEGORY_UTILS, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("countlinesmatching", DATA_TYPE_INT, COUNTLINESMATCHING_ARGS, &FnCallCountLinesMatching, "Count the number of lines matching regex arg1 in file arg2", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("difference", DATA_TYPE_STRING_LIST, SETOP_ARGS, &FnCallSetop, "Returns all the unique elements of list arg1 that are not in list arg2", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("dirname", DATA_TYPE_STRING, DIRNAME_ARGS, &FnCallDirname, "Return the parent directory name for given path", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("diskfree", DATA_TYPE_INT, DISKFREE_ARGS, &FnCallDiskFree, "Return the free space (in KB) available on the directory's current partition (0 if not found)", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("escape", DATA_TYPE_STRING, ESCAPE_ARGS, &FnCallEscape, "Escape regular expression characters in a string", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("eval", DATA_TYPE_STRING, EVAL_ARGS, &FnCallEval, "Evaluate a mathematical expression", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("every", DATA_TYPE_CONTEXT, EVERY_SOME_NONE_ARGS, &FnCallEverySomeNone, "True if every element in the named list matches the given regular expression", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("execresult", DATA_TYPE_STRING, EXECRESULT_ARGS, &FnCallExecResult, "Execute named command and assign output to variable", false, FNCALL_CATEGORY_UTILS, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("fileexists", DATA_TYPE_CONTEXT, FILESTAT_ARGS, &FnCallFileStat, "True if the named file can be accessed", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("filesexist", DATA_TYPE_CONTEXT, FILESEXIST_ARGS, &FnCallFileSexist, "True if the named list of files can ALL be accessed", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("filesize", DATA_TYPE_INT, FILESTAT_ARGS, &FnCallFileStat, "Returns the size in bytes of the file", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("filestat", DATA_TYPE_STRING, FILESTAT_DETAIL_ARGS, &FnCallFileStatDetails, "Returns stat() details of the file", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("filter", DATA_TYPE_STRING_LIST, FILTER_ARGS, &FnCallFilter, "Similarly to grep(), filter the list arg2 for matches to arg2.  The matching can be as a regular expression or exactly depending on arg3.  The matching can be inverted with arg4.  A maximum on the number of matches returned can be set with arg5.", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("findfiles", DATA_TYPE_STRING_LIST, FINDFILES_ARGS, &FnCallFindfiles, "Find files matching a shell glob pattern", true, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("format", DATA_TYPE_STRING, FORMAT_ARGS, &FnCallFormat, "Applies a list of string values in arg2,arg3... to a string format in arg1 with sprintf() rules", true, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("getenv", DATA_TYPE_STRING, GETENV_ARGS, &FnCallGetEnv, "Return the environment variable named arg1, truncated at arg2 characters", false, FNCALL_CATEGORY_SYSTEM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("getfields", DATA_TYPE_INT, GETFIELDS_ARGS, &FnCallGetFields, "Get an array of fields in the lines matching regex arg1 in file arg2, split on regex arg3 as array name arg4", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("getgid", DATA_TYPE_INT, GETGID_ARGS, &FnCallGetGid, "Return the integer group id of the named group on this host", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("getindices", DATA_TYPE_STRING_LIST, GETINDICES_ARGS, &FnCallGetIndices, "Get a list of keys to the array whose id is the argument and assign to variable", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("getuid", DATA_TYPE_INT, GETUID_ARGS, &FnCallGetUid, "Return the integer user id of the named user on this host", false, FNCALL_CATEGORY_SYSTEM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("getusers", DATA_TYPE_STRING_LIST, GETUSERS_ARGS, &FnCallGetUsers, "Get a list of all system users defined, minus those names defined in arg1 and uids in arg2", false, FNCALL_CATEGORY_SYSTEM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("getvalues", DATA_TYPE_STRING_LIST, GETINDICES_ARGS, &FnCallGetValues, "Get a list of values corresponding to the right hand sides in an array whose id is the argument and assign to variable", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("grep", DATA_TYPE_STRING_LIST, GREP_ARGS, &FnCallGrep, "Extract the sub-list if items matching the regular expression in arg1 of the list named in arg2", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("groupexists", DATA_TYPE_CONTEXT, GROUPEXISTS_ARGS, &FnCallGroupExists, "True if group or numerical id exists on this host", false, FNCALL_CATEGORY_SYSTEM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("hash", DATA_TYPE_STRING, HASH_ARGS, &FnCallHandlerHash, "Return the hash of arg1, type arg2 and assign to a variable", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("hashmatch", DATA_TYPE_CONTEXT, HASHMATCH_ARGS, &FnCallHashMatch, "Compute the hash of arg1, of type arg2 and test if it matches the value in arg3", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("host2ip", DATA_TYPE_STRING, HOST2IP_ARGS, &FnCallHost2IP, "Returns the primary name-service IP address for the named host", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("ip2host", DATA_TYPE_STRING, IP2HOST_ARGS, &FnCallIP2Host, "Returns the primary name-service host name for the IP address", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("hostinnetgroup", DATA_TYPE_CONTEXT, HOSTINNETGROUP_ARGS, &FnCallHostInNetgroup, "True if the current host is in the named netgroup", false, FNCALL_CATEGORY_SYSTEM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("hostrange", DATA_TYPE_CONTEXT, HOSTRANGE_ARGS, &FnCallHostRange, "True if the current host lies in the range of enumerated hostnames specified", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("hostsseen", DATA_TYPE_STRING_LIST, HOSTSSEEN_ARGS, &FnCallHostsSeen, "Extract the list of hosts last seen/not seen within the last arg1 hours", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("hostswithclass", DATA_TYPE_STRING_LIST, HOSTSWITHCLASS_ARGS, &FnCallHostsWithClass, "Extract the list of hosts with the given class set from the hub database (enterprise extension)", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("hubknowledge", DATA_TYPE_STRING, HUB_KNOWLEDGE_ARGS, &FnCallHubKnowledge, "Read global knowledge from the hub host by id (enterprise extension)", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("ifelse", DATA_TYPE_STRING, IFELSE_ARGS, &FnCallIfElse, "Do If-ElseIf-ElseIf-...-Else evaluation of arguments", true, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("intersection", DATA_TYPE_STRING_LIST, SETOP_ARGS, &FnCallSetop, "Returns all the unique elements of list arg1 that are also in list arg2", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("iprange", DATA_TYPE_CONTEXT, IPRANGE_ARGS, &FnCallIPRange, "True if the current host lies in the range of IP addresses specified", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("irange", DATA_TYPE_INT_RANGE, IRANGE_ARGS, &FnCallIRange, "Define a range of integer values for cfengine internal use", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("isdir", DATA_TYPE_CONTEXT, FILESTAT_ARGS, &FnCallFileStat, "True if the named object is a directory", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("isexecutable", DATA_TYPE_CONTEXT, FILESTAT_ARGS, &FnCallFileStat, "True if the named object has execution rights for the current user", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("isgreaterthan", DATA_TYPE_CONTEXT, ISGREATERTHAN_ARGS, &FnCallIsLessGreaterThan, "True if arg1 is numerically greater than arg2, else compare strings like strcmp", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("islessthan", DATA_TYPE_CONTEXT, ISLESSTHAN_ARGS, &FnCallIsLessGreaterThan, "True if arg1 is numerically less than arg2, else compare strings like NOT strcmp", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("islink", DATA_TYPE_CONTEXT, FILESTAT_ARGS, &FnCallFileStat, "True if the named object is a symbolic link", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("isnewerthan", DATA_TYPE_CONTEXT, ISNEWERTHAN_ARGS, &FnCallIsNewerThan, "True if arg1 is newer (modified later) than arg2 (mtime)", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("isplain", DATA_TYPE_CONTEXT, FILESTAT_ARGS, &FnCallFileStat, "True if the named object is a plain/regular file", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("isvariable", DATA_TYPE_CONTEXT, ISVARIABLE_ARGS, &FnCallIsVariable, "True if the named variable is defined", false, FNCALL_CATEGORY_UTILS, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("join", DATA_TYPE_STRING, JOIN_ARGS, &FnCallJoin, "Join the items of arg2 into a string, using the conjunction in arg1", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("lastnode", DATA_TYPE_STRING, LASTNODE_ARGS, &FnCallLastNode, "Extract the last of a separated string, e.g. filename from a path", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("laterthan", DATA_TYPE_CONTEXT, LATERTHAN_ARGS, &FnCallLaterThan, "True if the current time is later than the given date", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("ldaparray", DATA_TYPE_CONTEXT, LDAPARRAY_ARGS, &FnCallLDAPArray, "Extract all values from an ldap record", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("ldaplist", DATA_TYPE_STRING_LIST, LDAPLIST_ARGS, &FnCallLDAPList, "Extract all named values from multiple ldap records", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("ldapvalue", DATA_TYPE_STRING, LDAPVALUE_ARGS, &FnCallLDAPValue, "Extract the first matching named value from ldap", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("length", DATA_TYPE_INT, LENGTH_ARGS, &FnCallLength, "Return the length of a list", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("lsdir", DATA_TYPE_STRING_LIST, LSDIRLIST_ARGS, &FnCallLsDir, "Return a list of files in a directory matching a regular expression", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("maparray", DATA_TYPE_STRING_LIST, MAPARRAY_ARGS, &FnCallMapArray, "Return a list with each element modified by a pattern based $(this.k) and $(this.v)", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("maplist", DATA_TYPE_STRING_LIST, MAPLIST_ARGS, &FnCallMapList, "Return a list with each element modified by a pattern based $(this)", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("mergedata", DATA_TYPE_CONTAINER, MERGEDATA_ARGS, &FnCallMergeData, "", true, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("none", DATA_TYPE_CONTEXT, EVERY_SOME_NONE_ARGS, &FnCallEverySomeNone, "True if no element in the named list matches the given regular expression", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("not", DATA_TYPE_STRING, NOT_ARGS, &FnCallNot, "Calculate whether argument is false", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("now", DATA_TYPE_INT, NOW_ARGS, &FnCallNow, "Convert the current time into system representation", false, FNCALL_CATEGORY_SYSTEM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("nth", DATA_TYPE_STRING, NTH_ARGS, &FnCallNth, "Get the element at arg2 in list arg1", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("on", DATA_TYPE_INT, DATE_ARGS, &FnCallOn, "Convert an exact date/time to an integer system representation", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("or", DATA_TYPE_STRING, OR_ARGS, &FnCallOr, "Calculate whether any argument evaluates to true", true, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("parseintarray", DATA_TYPE_INT, PARSESTRINGARRAY_ARGS, &FnCallParseIntArray, "Read an array of integers from a file and assign the dimension to a variable", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("parsejson", DATA_TYPE_CONTAINER, PARSEJSON_ARGS, &FnCallParseJson, "", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("parserealarray", DATA_TYPE_INT, PARSESTRINGARRAY_ARGS, &FnCallParseRealArray, "Read an array of real numbers from a file and assign the dimension to a variable", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("parsestringarray", DATA_TYPE_INT, PARSESTRINGARRAY_ARGS, &FnCallParseStringArray, "Read an array of strings from a file and assign the dimension to a variable", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("parsestringarrayidx", DATA_TYPE_INT, PARSESTRINGARRAYIDX_ARGS, &FnCallParseStringArrayIndex, "Read an array of strings from a file and assign the dimension to a variable with integer indeces", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("peers", DATA_TYPE_STRING_LIST, PEERS_ARGS, &FnCallPeers, "Get a list of peers (not including ourself) from the partition to which we belong", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("peerleader", DATA_TYPE_STRING, PEERLEADER_ARGS, &FnCallPeerLeader, "Get the assigned peer-leader of the partition to which we belong", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("peerleaders", DATA_TYPE_STRING_LIST, PEERLEADERS_ARGS, &FnCallPeerLeaders, "Get a list of peer leaders from the named partitioning", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("product", DATA_TYPE_REAL, PRODUCT_ARGS, &FnCallProduct, "Return the product of a list of reals", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("randomint", DATA_TYPE_INT, RANDOMINT_ARGS, &FnCallRandomInt, "Generate a random integer between the given limits", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("readfile", DATA_TYPE_STRING, READFILE_ARGS, &FnCallReadFile, "Read max number of bytes from named file and assign to variable", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("readintarray", DATA_TYPE_INT, READSTRINGARRAY_ARGS, &FnCallReadIntArray, "Read an array of integers from a file and assign the dimension to a variable", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("readintlist", DATA_TYPE_INT_LIST, READSTRINGLIST_ARGS, &FnCallReadIntList, "Read and assign a list variable from a file of separated ints", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("readjson", DATA_TYPE_CONTAINER, READJSON_ARGS, &FnCallReadJson, "", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("readrealarray", DATA_TYPE_INT, READSTRINGARRAY_ARGS, &FnCallReadRealArray, "Read an array of real numbers from a file and assign the dimension to a variable", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("readreallist", DATA_TYPE_REAL_LIST, READSTRINGLIST_ARGS, &FnCallReadRealList, "Read and assign a list variable from a file of separated real numbers", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("readstringarray", DATA_TYPE_INT, READSTRINGARRAY_ARGS, &FnCallReadStringArray, "Read an array of strings from a file and assign the dimension to a variable", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("readstringarrayidx", DATA_TYPE_INT, READSTRINGARRAYIDX_ARGS, &FnCallReadStringArrayIndex, "Read an array of strings from a file and assign the dimension to a variable with integer indeces", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("readstringlist", DATA_TYPE_STRING_LIST, READSTRINGLIST_ARGS, &FnCallReadStringList, "Read and assign a list variable from a file of separated strings", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("readtcp", DATA_TYPE_STRING, READTCP_ARGS, &FnCallReadTcp, "Connect to tcp port, send string and assign result to variable", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("regarray", DATA_TYPE_CONTEXT, REGARRAY_ARGS, &FnCallRegArray, "True if arg1 matches any item in the associative array with id=arg2", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("regcmp", DATA_TYPE_CONTEXT, REGCMP_ARGS, &FnCallRegCmp, "True if arg1 is a regular expression matching that matches string arg2", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("regextract", DATA_TYPE_CONTEXT, REGEXTRACT_ARGS, &FnCallRegExtract, "True if the regular expression in arg 1 matches the string in arg2 and sets a non-empty array of backreferences named arg3", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("registryvalue", DATA_TYPE_STRING, REGISTRYVALUE_ARGS, &FnCallRegistryValue, "Returns a value for an MS-Win registry key,value pair", false, FNCALL_CATEGORY_SYSTEM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("regline", DATA_TYPE_CONTEXT, REGLINE_ARGS, &FnCallRegLine, "True if the regular expression in arg1 matches a line in file arg2", false, FNCALL_CATEGORY_IO, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("reglist", DATA_TYPE_CONTEXT, REGLIST_ARGS, &FnCallRegList, "True if the regular expression in arg2 matches any item in the list whose id is arg1", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("regldap", DATA_TYPE_CONTEXT, REGLDAP_ARGS, &FnCallRegLDAP, "True if the regular expression in arg6 matches a value item in an ldap search", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("remotescalar", DATA_TYPE_STRING, REMOTESCALAR_ARGS, &FnCallRemoteScalar, "Read a scalar value from a remote cfengine server", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("remoteclassesmatching", DATA_TYPE_CONTEXT, REMOTECLASSESMATCHING_ARGS, &FnCallRemoteClassesMatching, "Read persistent classes matching a regular expression from a remote cfengine server and add them into local context with prefix", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("returnszero", DATA_TYPE_CONTEXT, RETURNSZERO_ARGS, &FnCallReturnsZero, "True if named shell command has exit status zero", false, FNCALL_CATEGORY_UTILS, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("rrange", DATA_TYPE_REAL_RANGE, RRANGE_ARGS, &FnCallRRange, "Define a range of real numbers for cfengine internal use", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("reverse", DATA_TYPE_STRING_LIST, REVERSE_ARGS, &FnCallReverse, "Reverse a string list", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("selectservers", DATA_TYPE_INT, SELECTSERVERS_ARGS, &FnCallSelectServers, "Select tcp servers which respond correctly to a query and return their number, set array of names", false, FNCALL_CATEGORY_COMM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("shuffle", DATA_TYPE_STRING_LIST, SHUFFLE_ARGS, &FnCallShuffle, "Shuffle a string list", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("some", DATA_TYPE_CONTEXT, EVERY_SOME_NONE_ARGS, &FnCallEverySomeNone, "True if an element in the named list matches the given regular expression", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("sort", DATA_TYPE_STRING_LIST, SORT_ARGS, &FnCallSort, "Sort a string list", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("splayclass", DATA_TYPE_CONTEXT, SPLAYCLASS_ARGS, &FnCallSplayClass, "True if the first argument's time-slot has arrived, according to a policy in arg2", false, FNCALL_CATEGORY_UTILS, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("splitstring", DATA_TYPE_STRING_LIST, SPLITSTRING_ARGS, &FnCallSplitString, "Convert a string in arg1 into a list of max arg3 strings by splitting on a regular expression in arg2", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("storejson", DATA_TYPE_STRING, STOREJSON_ARGS, &FnCallStoreJson, "Convert a data container to a JSON string", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("strcmp", DATA_TYPE_CONTEXT, STRCMP_ARGS, &FnCallStrCmp, "True if the two strings match exactly", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("strftime", DATA_TYPE_STRING, STRFTIME_ARGS, &FnCallStrftime, "Format a date and time string", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("sublist", DATA_TYPE_STRING_LIST, SUBLIST_ARGS, &FnCallSublist, "Returns arg3 element from either the head or the tail (according to arg2) of list arg1.", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("sum", DATA_TYPE_REAL, SUM_ARGS, &FnCallSum, "Return the sum of a list of reals", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("translatepath", DATA_TYPE_STRING, TRANSLATEPATH_ARGS, &FnCallTranslatePath, "Translate path separators from Unix style to the host's native", false, FNCALL_CATEGORY_FILES, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("unique", DATA_TYPE_STRING_LIST, UNIQUE_ARGS, &FnCallUnique, "Returns all the unique elements of list arg1", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("usemodule", DATA_TYPE_CONTEXT, USEMODULE_ARGS, &FnCallUseModule, "Execute cfengine module script and set class if successful", false, FNCALL_CATEGORY_UTILS, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("userexists", DATA_TYPE_CONTEXT, USEREXISTS_ARGS, &FnCallUserExists, "True if user name or numerical id exists on this host", false, FNCALL_CATEGORY_SYSTEM, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("variablesmatching", DATA_TYPE_STRING_LIST, CLASSMATCH_ARGS, &FnCallVariablesMatching, "List the variables matching regex arg1 and tag regexes arg2,arg3,...", true, FNCALL_CATEGORY_UTILS, SYNTAX_STATUS_NORMAL),

    // Text xform functions
    FnCallTypeNew("downcase", DATA_TYPE_STRING, XFORM_ARGS, &FnCallTextXform, "Convert a string to lowercase", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("head", DATA_TYPE_STRING, XFORM_SUBSTR_ARGS, &FnCallTextXform, "Extract characters from the head of the string", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("reversestring", DATA_TYPE_STRING, XFORM_ARGS, &FnCallTextXform, "Reverse a string", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("strlen", DATA_TYPE_INT, XFORM_ARGS, &FnCallTextXform, "Return the length of a string", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("tail", DATA_TYPE_STRING, XFORM_SUBSTR_ARGS, &FnCallTextXform, "Extract characters from the tail of the string", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),
    FnCallTypeNew("upcase", DATA_TYPE_STRING, XFORM_ARGS, &FnCallTextXform, "Convert a string to UPPERCASE", false, FNCALL_CATEGORY_DATA, SYNTAX_STATUS_NORMAL),

    FnCallTypeNewNull()
};
