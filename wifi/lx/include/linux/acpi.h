/*
 * The phone has no ACPI. LinuxKPI's linux/acpi.h takes in FreeBSD's ACPICA, whose platform headers know no
 * Apple kernel; this is what ath10k's mac.c needs of it instead -- the regulatory domain it would read from
 * an ACPI table on a laptop (ath10k_mac_get_wrdd_regulatory()) -- answered with "there is no such thing".
 */
#ifndef _LX_LINUX_ACPI_H_
#define _LX_LINUX_ACPI_H_

#include <linux/types.h>

typedef void *acpi_handle;
typedef u32 acpi_status;
typedef char *acpi_string;

#define AE_OK                   0
#define AE_NOT_FOUND            5
#define ACPI_FAILURE(s)         ((s) != AE_OK)
#define ACPI_SUCCESS(s)         ((s) == AE_OK)
#define ACPI_ALLOCATE_BUFFER    ((size_t)-1)
#define ACPI_TYPE_INTEGER       1
#define ACPI_TYPE_PACKAGE       4
#define ACPI_HANDLE(dev)        ((acpi_handle)NULL)

struct acpi_buffer {
	size_t length;
	void *pointer;
};

union acpi_object {
	u32 type;
	struct {
		u32 type;
		u64 value;
	} integer;
	struct {
		u32 type;
		u32 count;
		union acpi_object *elements;
	} package;
};

static inline acpi_status
acpi_get_handle(acpi_handle parent __unused, acpi_string path __unused, acpi_handle *handle)
{
	*handle = NULL;
	return AE_NOT_FOUND;
}

static inline acpi_status
acpi_evaluate_object(acpi_handle handle __unused, acpi_string path __unused, void *arguments __unused,
    struct acpi_buffer *buffer __unused)
{
	return AE_NOT_FOUND;
}

#endif
