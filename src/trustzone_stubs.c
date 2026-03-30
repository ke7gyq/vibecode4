/*
 * TrustZone stub functions for FreeRTOS
 * These stubs provide empty implementations for TrustZone-related
 * functions when TrustZone is not used.
 */

#include <stddef.h>

/* Stub for SaveContext */
void SecureContext_SaveContext(void *xSecureContext, void *pxCurrentTCB)
{
    (void)xSecureContext;
    (void)pxCurrentTCB;
    /* No operation needed - TrustZone not in use */
}

/* Stub for LoadContext */
void SecureContext_LoadContext(void *xSecureContext)
{
    (void)xSecureContext;
    /* No operation needed - TrustZone not in use */
}

/* Stub for secure context variable */
void *xSecureContext = NULL;
