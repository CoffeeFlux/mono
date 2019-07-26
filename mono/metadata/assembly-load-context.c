#include "config.h"
#include "mono/metadata/assembly.h"
#include "mono/metadata/domain-internals.h"
#include "mono/metadata/icall-decl.h"
#include "mono/metadata/loader-internals.h"
#include "mono/metadata/loaded-images-internals.h"
#include "mono/utils/mono-error-internals.h"
#include "mono/utils/mono-logger-internals.h"

#ifdef ENABLE_NETCORE
/* MonoAssemblyLoadContext support only in netcore Mono */

static
GENERATE_GET_CLASS_WITH_CACHE_DECL (assembly_load_context);

GENERATE_GET_CLASS_WITH_CACHE (assembly_load_context, "System.Runtime.Loader", "AssemblyLoadContext");

void
mono_alc_init (MonoAssemblyLoadContext *alc, MonoDomain *domain)
{
	MonoLoadedImages *li = g_new0 (MonoLoadedImages, 1);
	mono_loaded_images_init (li, alc);
	alc->domain = domain;
	alc->loaded_images = li;
	alc->loaded_assemblies = NULL;
	mono_coop_mutex_init (&alc->assemblies_lock);
}

void
mono_alc_cleanup (MonoAssemblyLoadContext *alc)
{
	// TODO: get someone familiar with the GC to look at this
	GSList *tmp;
	MonoDomain *domain = alc->domain;

	mono_loaded_images_free (alc->loaded_images);

	for (tmp = alc->loaded_assemblies; tmp; tmp = tmp->next) {
		MonoAssembly *ass = (MonoAssembly *)tmp->data;
		mono_assembly_release_gc_roots (ass);
	}

	// Close dynamic assemblies first, since they have no ref count
	for (tmp = alc->loaded_assemblies; tmp; tmp = tmp->next) {
		MonoAssembly *ass = (MonoAssembly *)tmp->data;
		if (!ass->image || !image_is_dynamic (ass->image))
			continue;
		mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_ASSEMBLY, "Unloading ALC [%p], assembly %s[%p], ref_count=%d", alc, ass->aname.name, ass, ass->ref_count);
		if (!mono_assembly_close_except_image_pools (ass)) {
			mono_domain_assemblies_lock (domain);
			g_slist_remove (domain->domain_assemblies, ass);
			mono_domain_assemblies_unlock (domain);
			tmp->data = NULL;
		}
	}

	for (tmp = alc->loaded_assemblies; tmp; tmp = tmp->next) {
		MonoAssembly *ass = (MonoAssembly *)tmp->data;
		if (!ass)
			continue;
		if (!ass->image || image_is_dynamic (ass->image))
			continue;
		mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_ASSEMBLY, "Unloading domain %s[%p], assembly %s[%p], ref_count=%d", domain->friendly_name, domain, ass->aname.name, ass, ass->ref_count);
		if (!mono_assembly_close_except_image_pools (ass)) {
			mono_domain_assemblies_lock (domain);
			g_slist_remove (domain->domain_assemblies, ass);
			mono_domain_assemblies_unlock (domain);
			tmp->data = NULL;
		}
	}

	for (tmp = domain->domain_assemblies; tmp; tmp = tmp->next) {
		MonoAssembly *ass = (MonoAssembly *)tmp->data;
		if (ass) {
			mono_domain_assemblies_lock (domain);
			g_slist_remove (domain->domain_assemblies, ass);
			mono_domain_assemblies_unlock (domain);
			mono_assembly_close_finish (ass);
		}
	}

	g_slist_free (alc->loaded_assemblies);
	alc->loaded_assemblies = NULL;
	mono_coop_mutex_destroy (&alc->assemblies_lock);
}

void
mono_alc_assemblies_lock (MonoAssemblyLoadContext *alc)
{
	mono_coop_mutex_lock (&alc->assemblies_lock);
}

void
mono_alc_assemblies_unlock (MonoAssemblyLoadContext *alc)
{
	mono_coop_mutex_unlock (&alc->assemblies_lock);
}

gpointer
ves_icall_System_Runtime_Loader_AssemblyLoadContext_InternalInitializeNativeALC (gpointer this_gchandle_ptr, MonoBoolean is_default_alc, MonoBoolean collectible, MonoError *error)
{
	/* If the ALC is collectible, this_gchandle is weak, otherwise it's strong. */
	uint32_t this_gchandle = (uint32_t)GPOINTER_TO_UINT (this_gchandle_ptr);
	if (collectible) {
		mono_error_set_execution_engine (error, "Collectible AssemblyLoadContexts are not yet supported by MonoVM");
		return NULL;
	}

	MonoDomain *domain = mono_domain_get ();
	MonoAssemblyLoadContext *alc = NULL;

	if (is_default_alc) {
		alc = mono_domain_default_alc (domain);
		g_assert (alc);
		if (!alc->gchandle)
			alc->gchandle = this_gchandle;
	} else {
		/* create it */
		alc = mono_domain_create_individual_alc (domain, this_gchandle, collectible, error);
	}
	return alc;
}

gpointer
ves_icall_System_Runtime_Loader_AssemblyLoadContext_GetLoadContextForAssembly (MonoReflectionAssemblyHandle assm_obj, MonoError *error)
{
	MonoAssembly *assm = MONO_HANDLE_GETVAL (assm_obj, assembly);
	MonoAssemblyLoadContext *alc = mono_assembly_get_alc (assm);

	return GUINT_TO_POINTER (alc->gchandle);
}

gboolean
mono_alc_is_default (MonoAssemblyLoadContext *alc)
{
	return alc == mono_alc_domain (alc)->default_alc;
}

static MonoAssembly*
invoke_resolve_method (MonoMethod *resolve_method, MonoAssemblyLoadContext *alc, MonoAssemblyName *aname, MonoError *error)
{
	MonoAssembly *result = NULL;
	char* aname_str = NULL;
	HANDLE_FUNCTION_ENTER ();

	aname_str = mono_stringify_assembly_name (aname);

	MonoStringHandle aname_obj = mono_string_new_handle (mono_domain_get (), aname_str, error);
	goto_if_nok (error, leave);

	MonoReflectionAssemblyHandle assm;
	gpointer args[2];
	args [0] = GUINT_TO_POINTER (alc->gchandle);
	args [1] = MONO_HANDLE_RAW (aname_obj);
	assm = MONO_HANDLE_CAST (MonoReflectionAssembly, mono_runtime_try_invoke_handle (resolve_method, NULL_HANDLE, args, error));
	goto_if_nok (error, leave);

	if (MONO_HANDLE_BOOL (assm))
		result = MONO_HANDLE_GETVAL (assm, assembly);

leave:
	g_free (aname_str);
	HANDLE_FUNCTION_RETURN_VAL (result);
}

static MonoAssembly*
mono_alc_invoke_resolve_using_load (MonoAssemblyLoadContext *alc, MonoAssemblyName *aname, MonoError *error)
{
	static MonoMethod *resolve;

	if (!resolve) {
		ERROR_DECL (local_error);
		MonoClass *alc_class = mono_class_get_assembly_load_context_class ();
		g_assert (alc_class);
		MonoMethod *m = mono_class_get_method_from_name_checked (alc_class, "MonoResolveUsingLoad", -1, 0, local_error);
		mono_error_assert_ok (local_error);
		resolve = m;
	}
	g_assert (resolve);

	return invoke_resolve_method (resolve, alc, aname, error);
}

MonoAssembly*
mono_alc_invoke_resolve_using_load_nofail (MonoAssemblyLoadContext *alc, MonoAssemblyName *aname)
{
	MonoAssembly *result = NULL;
	ERROR_DECL (error);

	result = mono_alc_invoke_resolve_using_load (alc, aname, error);
	if (!is_ok (error))
		mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_ASSEMBLY, "Error while invoking ALC Load(\"%s\") method: '%s'", aname->name, mono_error_get_message (error));

	mono_error_cleanup (error);

	return result;
}

static MonoAssembly*
mono_alc_invoke_resolve_using_resolving_event (MonoAssemblyLoadContext *alc, MonoAssemblyName *aname, MonoError *error)
{
	static MonoMethod *resolve;

	if (!resolve) {
		ERROR_DECL (local_error);
		MonoClass *alc_class = mono_class_get_assembly_load_context_class ();
		g_assert (alc_class);
		MonoMethod *m = mono_class_get_method_from_name_checked (alc_class, "MonoResolveUsingResolvingEvent", -1, 0, local_error);
		mono_error_assert_ok (local_error);
		resolve = m;
	}
	g_assert (resolve);

	return invoke_resolve_method (resolve, alc, aname, error);
}

MonoAssembly*
mono_alc_invoke_resolve_using_resolving_event_nofail (MonoAssemblyLoadContext *alc, MonoAssemblyName *aname)
{
	MonoAssembly *result = NULL;
	ERROR_DECL (error);

	result = mono_alc_invoke_resolve_using_resolving_event (alc, aname, error);
	if (!is_ok (error))
		mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_ASSEMBLY, "Error while invoking ALC Resolving(\"%s\") event: '%s'", aname->name, mono_error_get_message (error));

	mono_error_cleanup (error);

	return result;
}

static MonoAssembly*
mono_alc_invoke_resolve_using_resolve_satellite (MonoAssemblyLoadContext *alc, MonoAssemblyName *aname, MonoError *error)
{
	static MonoMethod *resolve;

	if (!resolve) {
		ERROR_DECL (local_error);
		MonoClass *alc_class = mono_class_get_assembly_load_context_class ();
		g_assert (alc_class);
		MonoMethod *m = mono_class_get_method_from_name_checked (alc_class, "MonoResolveUsingResolveSatelliteAssembly", -1, 0, local_error);
		mono_error_assert_ok (local_error);
		resolve = m;
	}
	g_assert (resolve);

	return invoke_resolve_method (resolve, alc, aname, error);
}

MonoAssembly*
mono_alc_invoke_resolve_using_resolve_satellite_nofail (MonoAssemblyLoadContext *alc, MonoAssemblyName *aname)
{
	MonoAssembly *result = NULL;
	ERROR_DECL (error);

	result = mono_alc_invoke_resolve_using_resolve_satellite (alc, aname, error);
	if (!is_ok (error))
		mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_ASSEMBLY, "Error while invoking ALC ResolveSatelliteAssembly(\"%s\") method: '%s'", aname->name, mono_error_get_message (error));

	mono_error_cleanup (error);

	return result;
}



#endif /* ENABLE_NETCORE */
