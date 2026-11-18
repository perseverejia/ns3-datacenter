#include "cdf.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

/* initialize a CDF distribution */
void init_cdf(struct cdf_table *table)
{
    if(!table)
        return;

    table->entries = (struct cdf_entry*)malloc(TG_CDF_TABLE_ENTRY * sizeof(struct cdf_entry));
    table->num_entry = 0;
    table->max_entry = TG_CDF_TABLE_ENTRY;
    table->min_cdf = 0;
    table->max_cdf = 1;

    if (!(table->entries))
        perror("Error: malloc entries in init_cdf()");
}

/* free resources of a CDF distribution */
void free_cdf(struct cdf_table *table)
{
    if (table)
        free(table->entries);
}

/* get CDF distribution from a given file */
int load_cdf(struct cdf_table *table, const char *file_name)
{
    FILE *fd = NULL;
    char line[256] = {0};
    struct cdf_entry *e = NULL;
    int has_whole_byte = 0;

    if (!table || !table->entries)
        return 0;

    fd = fopen(file_name, "r");
    if (!fd)
        return 0;

    while (fgets(line, sizeof(line), fd))
    {
        char *row = line;
        while (isspace((unsigned char)*row)) row++;
        if (*row == '\0' || *row == '#') continue;
        struct cdf_entry point;
        struct cdf_entry previous = {0, 0};
        if (table->num_entry) previous = table->entries[table->num_entry - 1];
        if (sscanf(row, "%lf %lf", &point.value, &point.cdf) != 2 ||
            !isfinite(point.value) || point.value < previous.value || point.value > UINT32_MAX ||
            !isfinite(point.cdf) || point.cdf < previous.cdf || point.cdf > 1)
        {
            fclose(fd);
            return 0;
        }
        if (point.cdf > previous.cdf && (point.value > 1 || previous.value >= 1))
            has_whole_byte = 1;
        /* resize entries */
        if (table->num_entry >= table->max_entry)
        {
            if (table->max_entry > INT_MAX / 2) {
                fclose(fd);
                return 0;
            }
            e = realloc(table->entries, 2u * table->max_entry * sizeof(*e));
            if (!e) {
                fclose(fd);
                return 0;
            }
            table->entries = e;
            table->max_entry *= 2;
        }

        table->entries[table->num_entry++] = point;
    }
    if (table->num_entry > 0) {
        struct cdf_entry last = table->entries[table->num_entry - 1];
        if (last.cdf < 1 && last.value >= 1) has_whole_byte = 1;
    }
    int valid = !ferror(fd) && has_whole_byte;
    fclose(fd);
    return valid;
}

/* print CDF distribution information */
void print_cdf(struct cdf_table *table)
{
    int i = 0;

    if (!table)
        return;

    for (i = 0; i < table->num_entry; i++)
        printf("%.2f %.2f\n", table->entries[i].value, table->entries[i].cdf);
}

/* get average value of CDF distribution */
double avg_cdf(struct cdf_table *table)
{
    int i = 0;
    double avg = 0;
    double value, prob;

    if (!table)
        return 0;

    for (i = 0; i < table->num_entry; i++)
    {
        if (i == 0)
        {
            value = table->entries[i].value / 2;
            prob = table->entries[i].cdf;
        }
        else
        {
            value = (table->entries[i].value + table->entries[i-1].value) / 2;
            prob = table->entries[i].cdf - table->entries[i-1].cdf;
        }
        avg += (value * prob);
    }
    if (table->num_entry > 0) {
        struct cdf_entry last = table->entries[table->num_entry - 1];
        avg += (1 - last.cdf) * last.value;
    }

    return avg;
}

double interpolate(double x, double x1, double y1, double x2, double y2)
{
    if (x1 == x2)
        return (y1 + y2) / 2;
    else
        return y1 + (x - x1) * (y2 - y1) / (x2 - x1);
}

/* generate a random floating point number from min to max */
double rand_range(double min, double max)
{
    return min + rand() * (max - min) / RAND_MAX;
}

/* generate a random value based on CDF distribution */
double gen_random_cdf(struct cdf_table *table)
{
    if (!table)
        return 0;
    return gen_value_cdf(table, rand_range(table->min_cdf, table->max_cdf));
}

double gen_value_cdf(struct cdf_table *table, double quantile)
{
    int i = 0;

    if (!table || table->num_entry == 0)
        return 0;

    for (i = 0; i < table->num_entry; i++)
    {
        if (quantile <= table->entries[i].cdf)
        {
            if (i == 0)
                return interpolate(quantile, 0, 0, table->entries[i].cdf, table->entries[i].value);
            else
                return interpolate(quantile, table->entries[i-1].cdf, table->entries[i-1].value, table->entries[i].cdf, table->entries[i].value);
        }
    }

    return table->entries[table->num_entry-1].value;
}
