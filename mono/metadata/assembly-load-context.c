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
	/*
	 * As it stands, ALC and domain cleanup is probably broken on netcore. Without ALC collectability, this should not
	 * be hit. I've documented roughly the things that still need to be accomplish, but the implementation is TODO and
	 * the ideal order and locking unclear.
	 * 
	 * For now, this makes two important assumptions:
	 *   1. The minimum refcount on assemblies is 2: one for the domain and one for the ALC. The domain refcount might 
	 *        be less than optimal on netcore, but its removal is too likely to cause issues for now.
	 *   2. An ALC will have been removed from the domain before cleanup.
	 */
	GSList *tmp;
	MonoDomain *domain = alc->domain;

	/*
	 * Missing steps:
	 * 
	 * + Release GC roots for all assemblies in the ALC
	 * + Iterate over the domain_assemblies and remove ones that belong to the ALC, which will probably require
	 *     converting domain_assemblies to a doubly-linked list, ideally GQueue
	 * + Close dynamic and then remaining assemblies, potentially nulling the data field depending on refcount
	 * + Second pass to call mono_assembly_close_finish on remaining assemblies
	 */

	mono_loaded_images_free (alc->loaded_images);

	g_slist_free (alc->loaded_assemblies);
	alc->loaded_assemblies = NULL;
	mono_coop_mutex_destroy (&alc->assemblies_lock);

	g_assert_not_reached ();
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
