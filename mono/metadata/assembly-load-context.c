#include "config.h"
#include "mono/metadata/domain-internals.h"
#include "mono/metadata/icall-decl.h"
#include "mono/metadata/loader-internals.h"
#include "mono/metadata/loaded-images-internals.h"
#include "mono/utils/mono-error-internals.h"
#include "mono/utils/mono-logger-internals.h"

#ifdef ENABLE_NETCORE
/* MonoAssemblyLoadContext support only in netcore Mono */

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
	} else {
		/* create it */
		alc = mono_domain_create_individual_alc (domain, this_gchandle, collectible, error);
	}
	return alc;
}

#endif /* ENABLE_NETCORE */
