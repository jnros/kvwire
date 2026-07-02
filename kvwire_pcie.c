#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "kvwire.h"

static int read_attr(const char *dir, const char *attr, char *buf, size_t n)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", dir, attr);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, n, f)) { fclose(f); return -1; }
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

static int has_link(const char *dir)
{
	char path[512];
	snprintf(path, sizeof path, "%s/current_link_speed", dir);
	return access(path, R_OK) == 0;
}

/* rxe et al ride a real netdev; hop to it when the ib device has no bus. */
static int resolve_pci_dir(const char *ibdev, char *dir, size_t dn,
			   char *via, size_t vn)
{
	char base[128], net[24];

	via[0] = '\0';
	snprintf(dir, dn, "/sys/class/infiniband/%s/device", ibdev);
	if (has_link(dir))
		return 0;

	snprintf(base, sizeof base, "/sys/class/infiniband/%s", ibdev);
	if (read_attr(base, "parent", net, sizeof net) < 0)
		return -1;
	snprintf(dir, dn, "/sys/class/net/%s/device", net);
	if (!has_link(dir))
		return -1;
	snprintf(via, vn, "%s", net);
	return 0;
}

/* gen1/2: 8b/10b; gen3+: 128b/130b */
static double pcie_ceiling(const char *link_speed, int width)
{
	double gts = atof(link_speed);
	double eff = gts >= 8.0 ? 128.0 / 130.0 : 0.8;
	return gts * eff * width;
}

int pcie_query(const char *ibdev, struct pcie_info *out)
{
	char dir[256];
	char w[32];

	memset(out, 0, sizeof *out);
	out->numa = -1;

	if (resolve_pci_dir(ibdev, dir, sizeof dir, out->via, sizeof out->via) < 0)
		return -1;

	if (read_attr(dir, "current_link_speed", out->link_speed,
		      sizeof out->link_speed) < 0)
		return -1;
	if (read_attr(dir, "current_link_width", w, sizeof w) < 0)
		return -1;
	out->width = atoi(w);

	read_attr(dir, "vendor", out->vendor, sizeof out->vendor);
	read_attr(dir, "device", out->device, sizeof out->device);
	if (read_attr(dir, "numa_node", w, sizeof w) == 0)
		out->numa = atoi(w);

	char real[PATH_MAX];
	if (realpath(dir, real)) {
		char *slash = strrchr(real, '/');
		snprintf(out->bdf, sizeof out->bdf, "%.31s", slash ? slash + 1 : real);
	}

	out->ceiling_gbps = pcie_ceiling(out->link_speed, out->width);
	out->ok = 1;
	return 0;
}

/* one line, to stderr, so it rides along with any run without touching stdout.
 * silent when the probe misses - only --probe reports that loudly. */
void pcie_oneline(const char *ibdev, const struct pcie_info *pi)
{
	if (!pi->ok)
		return;
	fprintf(stderr, "pcie: %s %s  %s x%d = %.1f Gb/s ceiling%s%s\n",
		ibdev, pi->bdf, pi->link_speed, pi->width, pi->ceiling_gbps,
		pi->via[0] ? "  via netdev " : "", pi->via);
}

void pcie_print(const char *ibdev, const struct pcie_info *pi)
{
	if (!pi->ok) {
		printf("pcie: %s has no PCIe function in sysfs\n", ibdev);
		return;
	}
	printf("pcie: %s  PCIe %s  vendor=%s device=%s  numa=%d%s%s\n",
	       ibdev, pi->bdf, pi->vendor, pi->device, pi->numa,
	       pi->via[0] ? "  via netdev " : "", pi->via);
	printf("pcie: link  %s  x%d  ->  %.1f Gb/s payload ceiling\n",
	       pi->link_speed, pi->width, pi->ceiling_gbps);
}
