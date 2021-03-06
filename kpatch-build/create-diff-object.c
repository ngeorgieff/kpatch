/*
 * create-diff-object.c
 *
 * Copyright (C) 2014 Seth Jennings <sjenning@redhat.com>
 * Copyright (C) 2013 Josh Poimboeuf <jpoimboe@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA,
 * 02110-1301, USA.
 */

/*
 * This file contains the heart of the ELF object differencing engine.
 *
 * The tool takes two ELF objects from two versions of the same source
 * file; a "base" object and a "patched" object.  These object need to have
 * been compiled with the -ffunction-sections and -fdata-sections GCC options.
 *
 * The tool compares the objects at a section level to determine what
 * sections have changed.  Once a list of changed sections has been generated,
 * various rules are applied to determine any object local sections that
 * are dependencies of the changed section and also need to be included in
 * the output object.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <error.h>
#include <gelf.h>
#include <argp.h>

#define ERROR(format, ...) \
	error(1, 0, "%s: %d: " format, __FUNCTION__, __LINE__, ##__VA_ARGS__)

#define DIFF_FATAL(format, ...) \
({ \
	printf("%s:%d: " format "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__); \
	error(2, 0, "unreconcilable difference"); \
})

#define log_debug(format, ...) log(DEBUG, format, ##__VA_ARGS__)
#define log_normal(format, ...) log(NORMAL, format, ##__VA_ARGS__)

#define log(level, format, ...) \
({ \
	if (loglevel <= (level)) \
		printf(format, ##__VA_ARGS__); \
})


enum loglevel {
	DEBUG,
	NORMAL
};

static enum loglevel loglevel = NORMAL;

/*******************
 * Data structures
 * ****************/
struct section;
struct symbol;
struct rela;

enum status {
	NEW,
	CHANGED,
	SAME
};

struct table {
	void *data;
	size_t nr;
};

struct section {
	struct section *twin, *twino;
	GElf_Shdr sh;
	Elf_Data *data;
	char *name;
	int index;
	enum status status;
	int include;
	union {
		struct { /* if (is_rela_section()) */
			struct section *base;
			struct table relas;
		};
		struct { /* else */
			struct section *rela;
			struct symbol *secsym, *sym;
		};
	};
};

struct symbol {
	struct symbol *twin, *twino;
	struct section *sec;
	GElf_Sym sym;
	char *name;
	int index;
	unsigned char bind, type;
	enum status status;
	int include;
};

struct rela {
	struct rela *twin;
	GElf_Rela rela;
	struct symbol *sym;
	unsigned char type;
	int addend;
	int offset;
	char *string;
	enum status status;
};

#define for_each_entry(iter, entry, table, type) \
	for (iter = 0; (iter) < (table)->nr && ((entry) = &((type)(table)->data)[iter]); (iter)++)

#define for_each_section(iter, entry, table) \
	for_each_entry(iter, entry, table, struct section *)
#define for_each_symbol(iter, entry, table) \
	for_each_entry(iter, entry, table, struct symbol *)
#define for_each_rela(iter, entry, table) \
	for_each_entry(iter, entry, table, struct rela *)

struct kpatch_elf {
	Elf *elf;
	struct table sections;
	struct table symbols;
};

/*******************
 * Helper functions
 ******************/

char *status_str(enum status status)
{
	switch(status) {
	case NEW:
		return "NEW";
	case CHANGED:
		return "CHANGED";
	case SAME:
		return "SAME";
	default:
		ERROR("status_str");
	}
	/* never reached */
	return NULL;
}

int is_rela_section(struct section *sec)
{
	return (sec->sh.sh_type == SHT_RELA);
}

struct section *find_section_by_index(struct table *table, unsigned int index)
{
	struct section *sec;
	int i;

	for_each_section(i, sec, table)
		if (sec->index == index)
			return sec;

	return NULL;
}

struct section *find_section_by_name(struct table *table, const char *name)
{
	struct section *sec;
	int i;

	for_each_section(i, sec, table)
		if (!strcmp(sec->name, name))
			return sec;

	return NULL;
}

struct symbol *find_symbol_by_index(struct table *table, size_t index)
{
	struct symbol *sym;
	int i;

	for_each_symbol(i, sym, table)
		if (sym->index == index)
			return sym;

	return NULL;
}

struct symbol *find_symbol_by_name(struct table *table, const char *name)
{
	struct symbol *sym;
	int i;

	for_each_symbol(i, sym, table)
		if (sym->name && !strcmp(sym->name, name))
			return sym;

	return NULL;
}

void alloc_table(struct table *table, size_t entsize, size_t nr)
{
	size_t size = nr * entsize;

	table->data = malloc(size);
	if (!table->data)
		ERROR("malloc");
	memset(table->data, 0, size);
	table->nr = nr;
}

/*************
 * Functions
 * **********/
void kpatch_create_rela_table(struct kpatch_elf *kelf, struct section *sec)
{
	int rela_nr, i;
	struct rela *rela;
	unsigned int symndx;

	/* find matching base (text/data) section */
	sec->base = find_section_by_name(&kelf->sections, sec->name + 5);
	if (!sec->base)
		ERROR("can't find base section for rela section %s", sec->name);

	/* create reverse link from base section to this rela section */
	sec->base->rela = sec;
		
	/* allocate rela table for section */
	rela_nr = sec->sh.sh_size / sec->sh.sh_entsize;
	alloc_table(&sec->relas, sizeof(struct rela), rela_nr);

	log_debug("\n=== rela table for %s (%d entries) ===\n",
		sec->base->name, rela_nr);

	/* read and store the rela entries */
	for_each_rela(i, rela, &sec->relas) {
		if (!gelf_getrela(sec->data, i, &rela->rela))
			ERROR("gelf_getrela");

		rela->type = GELF_R_TYPE(rela->rela.r_info);
		rela->addend = rela->rela.r_addend;
		rela->offset = rela->rela.r_offset;
		symndx = GELF_R_SYM(rela->rela.r_info);
		rela->sym = find_symbol_by_index(&kelf->symbols, symndx);
		if (!rela->sym)
			ERROR("could not find rela entry symbol\n");
		if (rela->sym->sec && (rela->sym->sec->sh.sh_flags & SHF_STRINGS)) {
			rela->string = rela->sym->sec->data->d_buf + rela->addend;
			if (!rela->string)
				ERROR("could not lookup rela string\n");
		}

		log_debug("offset %d, type %d, %s %s %d", rela->offset,
			rela->type, rela->sym->name,
			(rela->addend < 0)?"-":"+", abs(rela->addend));
		if (rela->string)
			log_debug(" (string = %s)", rela->string);
		log_debug("\n");
	}
}

void kpatch_create_section_table(struct kpatch_elf *kelf)
{
	Elf_Scn *scn = NULL;
	struct section *sec;
	size_t shstrndx, sections_nr;
	int i;

	if (elf_getshdrnum(kelf->elf, &sections_nr))
		ERROR("elf_getshdrnum");

	/*
	 * elf_getshdrnum() includes section index 0 but elf_nextscn
	 * doesn't return that section so subtract one.
	 */
	sections_nr--;

	alloc_table(&kelf->sections, sizeof(struct section), sections_nr);

	if (elf_getshdrstrndx(kelf->elf, &shstrndx))
		ERROR("elf_getshdrstrndx");

	log_debug("=== section list (%zu) ===\n", sections_nr);

	for_each_section(i, sec, &kelf->sections) {
		scn = elf_nextscn(kelf->elf, scn);
		if (!scn)
			ERROR("scn NULL");

		if (!gelf_getshdr(scn, &sec->sh))
			ERROR("gelf_getshdr");

		sec->name = elf_strptr(kelf->elf, shstrndx, sec->sh.sh_name);
		if (!sec->name)
			ERROR("elf_strptr");

		sec->data = elf_getdata(scn, NULL);
		if (!sec->data)
			ERROR("elf_getdata");

		sec->index = elf_ndxscn(scn);

		log_debug("ndx %02d, data %p, size %zu, name %s\n",
			sec->index, sec->data->d_buf, sec->data->d_size,
			sec->name);
	}

	/* Sanity check, one more call to elf_nextscn() should return NULL */
	if (elf_nextscn(kelf->elf, scn))
		ERROR("expected NULL");
}

void kpatch_create_symbol_table(struct kpatch_elf *kelf)
{
	struct section *symtab;
	struct symbol *sym;
	int symbols_nr, i;

	symtab = find_section_by_name(&kelf->sections, ".symtab");
	if (!symtab)
		ERROR("missing symbol table");

	symbols_nr = symtab->sh.sh_size / symtab->sh.sh_entsize;

	alloc_table(&kelf->symbols, sizeof(struct symbol), symbols_nr);

	log_debug("\n=== symbol table (%d entries) ===\n", symbols_nr);

	/* iterator i declared in for_each_entry() macro */
	for_each_symbol(i, sym, &kelf->symbols) {
		if (i == 0) /* skip symbol 0 */
			continue;
		sym->index = i;

		if (!gelf_getsym(symtab->data, i, &sym->sym))
			ERROR("gelf_getsym");

		sym->name = elf_strptr(kelf->elf, symtab->sh.sh_link,
				       sym->sym.st_name);
		if (!sym->name)
			ERROR("elf_strptr");

		sym->type = GELF_ST_TYPE(sym->sym.st_info);
		sym->bind = GELF_ST_BIND(sym->sym.st_info);

		if (sym->sym.st_shndx > SHN_UNDEF &&
		    sym->sym.st_shndx < SHN_LORESERVE) {
			sym->sec = find_section_by_index(&kelf->sections,
					sym->sym.st_shndx);
			if (!sym->sec)
				ERROR("couldn't find section for symbol %s\n",
					sym->name);

			/*
			 * __ksymtab_strings is a special case where the
			 * compiler creates FUNC/OBJECT syms that refer
			 * to offsets inside the __ksymtab_strings section
			 * for kernel exported symbols.  We want to ignore
			 * those.
			 */
			if ((sym->type == STT_FUNC ||
			     sym->type == STT_OBJECT) &&
			    strcmp(sym->sec->name, "__ksymtab_strings")) {
				if (sym->sym.st_value != 0)
					ERROR("symbol %s at offset %lu within section %s, expected 0",
					      sym->name, sym->sym.st_value, sym->sec->name);
				sym->sec->sym = sym;
			} else if (sym->type == STT_SECTION) {
				sym->sec->secsym = sym;
				/* use the section name as the symbol name */
				sym->name = sym->sec->name;
			}
		}

		log_debug("sym %02d, type %d, bind %d, ndx %02d, name %s",
			sym->index, sym->type, sym->bind, sym->sym.st_shndx,
			sym->name);
		if (sym->sec)
			log_debug(" -> %s", sym->sec->name);
		log_debug("\n");
	}

}


struct kpatch_elf *kpatch_elf_open(const char *name)
{
	Elf *elf;
	int fd, i;
	struct kpatch_elf *kelf;
	struct section *sec;

	fd = open(name, O_RDONLY);
	if (fd == -1)
		ERROR("open");

	elf = elf_begin(fd, ELF_C_READ_MMAP, NULL);
	if (!elf)
		ERROR("elf_begin");

	kelf = malloc(sizeof(*kelf));
	if (!kelf)
		ERROR("malloc");
	memset(kelf, 0, sizeof(*kelf));

	/* read and store section, symbol entries from file */
	kelf->elf = elf;
	kpatch_create_section_table(kelf);
	kpatch_create_symbol_table(kelf);

	/* for each rela section, read and store the rela entries */
	for_each_section(i, sec, &kelf->sections) {
		if (!is_rela_section(sec))
			continue;
		kpatch_create_rela_table(kelf, sec);
	}

	return kelf;
}

void kpatch_compare_correlated_nonrela_section(struct section *sec)
{
	struct section *sec1 = sec, *sec2 = sec->twin;

	/* Compare section headers (must match or fatal) */
	if (sec1->sh.sh_type != sec2->sh.sh_type ||
	    sec1->sh.sh_flags != sec2->sh.sh_flags ||
	    sec1->sh.sh_addr != sec2->sh.sh_addr ||
	    sec1->sh.sh_addralign != sec2->sh.sh_addralign ||
	    sec1->sh.sh_entsize != sec2->sh.sh_entsize ||
	    sec1->sh.sh_link != sec1->sh.sh_link)
		DIFF_FATAL("%s section header details differ", sec1->name);

	if (sec1->sh.sh_size != sec2->sh.sh_size ||
	    sec1->data->d_size != sec2->data->d_size || 
	    (sec1->sh.sh_type != SHT_NOBITS &&
	     memcmp(sec1->data->d_buf, sec2->data->d_buf, sec1->data->d_size)))
		sec1->status = CHANGED;
	else
		sec1->status = SAME;
}

void kpatch_compare_correlated_nonrela_sections(struct table *table)
{
	struct section *sec;
	int i;

	for_each_section(i, sec, table) {
		if (is_rela_section(sec))
			continue;
		if (sec->twin)
			kpatch_compare_correlated_nonrela_section(sec);
		else
			sec->status = NEW;

		/* sync any rela section and associated symbols */
		if (sec->sym)
			sec->sym->status = sec->status;
		if (sec->secsym)
			sec->secsym->status = sec->status;
		if (sec->rela)
			sec->rela->status = sec->status;
	}
}

void kpatch_compare_correlated_symbol(struct symbol *sym)
{
	struct symbol *sym1 = sym, *sym2 = sym->twin;

	if (sym1->sym.st_info != sym2->sym.st_info ||
	    sym1->sym.st_other != sym2->sym.st_other ||
	    (sym1->sec && sym2->sec && sym1->sec->twin != sym2->sec) ||
	    (sym1->sec && !sym2->sec) ||
	    (sym2->sec && !sym1->sec))
		DIFF_FATAL("symbol info mismatch: %s", sym1->name);

	if (sym1->type == STT_OBJECT &&
	    sym1->sym.st_size != sym2->sym.st_size)
		DIFF_FATAL("object size mismatch: %s", sym1->name);

	if (sym1->sym.st_shndx == SHN_UNDEF ||
	     sym1->sym.st_shndx == SHN_ABS)
		sym1->status = SAME;
}

void kpatch_compare_correlated_symbols(struct table *table)
{
	struct symbol *sym;
	int i;

	for_each_symbol(i, sym, table) {
		if (i == 0) /* ugh */
			continue;
		if (sym->twin)
			kpatch_compare_correlated_symbol(sym);
		else
			sym->status = NEW;

		log_debug("symbol %s is %s\n", sym->name, status_str(sym->status));
	}
}

void kpatch_correlate_sections(struct table *table1, struct table *table2)
{
	struct section *sec1, *sec2;
	int i, j;

	/* correlate all sections and compare nonrela sections */
	for_each_section(i, sec1, table1) {
		for_each_section(j, sec2, table2) {
			if (strcmp(sec1->name, sec2->name))
				continue;
			sec1->twin = sec2;
			sec2->twin = sec1;
			/* set initial status, might change */
			sec1->status = sec2->status = SAME;
			break;
		}
	}
}

void kpatch_correlate_symbols(struct table *table1, struct table *table2)
{
	struct symbol *sym1, *sym2;
	int i, j;

	for_each_symbol(i, sym1, table1) {
		if (i == 0) /* ugh */
			continue;
		for_each_symbol(j, sym2, table2) {
			if (j == 0) /* double ugh */
				continue;
			if (!strcmp(sym1->name, sym2->name)) {
				sym1->twin = sym2;
				sym2->twin = sym1;
				/* set initial status, might change */
				sym1->status = sym2->status = SAME;
				break;
			}
		}
	}
}

int rela_equal(struct rela *rela1, struct rela *rela2)
{
	if (rela1->type != rela2->type ||
	    rela1->offset != rela2->offset)
		return 0;

	if (rela1->string) {
		if (rela2->string &&
		    !strcmp(rela1->string, rela2->string))
			return 1;
	} else {
		if (strcmp(rela1->sym->name, rela2->sym->name))
			return 0;
		if (rela1->addend == rela2->addend)
			return 1;
	}

	return 0;
}

void kpatch_correlate_relas(struct section *sec)
{
	struct rela *rela1, *rela2;
	int i, j;

	for_each_rela(i, rela1, &sec->relas) {
		for_each_rela(j, rela2, &sec->twin->relas) {
			    if (rela_equal(rela1, rela2)) {
				rela1->twin = rela2;
				rela2->twin = rela1;
				rela1->status = rela2->status = SAME;
				break;
			}
		}
	}
}

void kpatch_compare_elf_headers(Elf *elf1, Elf *elf2)
{
	GElf_Ehdr eh1, eh2;

	if (!gelf_getehdr(elf1, &eh1))
		ERROR("gelf_getehdr");

	if (!gelf_getehdr(elf2, &eh2))
		ERROR("gelf_getehdr");

	if (memcmp(eh1.e_ident, eh2.e_ident, EI_NIDENT) ||
	    eh1.e_type != eh2.e_type ||
	    eh1.e_machine != eh2.e_machine ||
	    eh1.e_version != eh2.e_version ||
	    eh1.e_entry != eh2.e_entry ||
	    eh1.e_phoff != eh2.e_phoff ||
	    eh1.e_flags != eh2.e_flags ||
	    eh1.e_ehsize != eh2.e_ehsize ||
	    eh1.e_phentsize != eh2.e_phentsize ||
	    eh1.e_shentsize != eh2.e_shentsize)
		DIFF_FATAL("ELF headers differ");
}

void kpatch_check_program_headers(Elf *elf)
{
	size_t ph_nr;

	if (elf_getphdrnum(elf, &ph_nr))
		ERROR("elf_getphdrnum");

	if (ph_nr != 0)
		DIFF_FATAL("ELF contains program header");
}

void kpatch_set_rela_section_status(struct section *sec)
{
	struct rela *rela;
	int i;

	for_each_rela(i, rela, &sec->relas)
		if (rela->status == NEW) {
			/*
			 * This rela section is different. Make
			 * sure the text section and any associated
			 * symbols come along too.
			 */
			sec->status = CHANGED;
			sec->base->status = CHANGED;
			if (sec->base->sym)
				sec->base->sym->status = CHANGED;
			if (sec->base->secsym)
				sec->base->secsym->status = CHANGED;
			return;
		}

	/*
	 * The difference in the section data was due to the renumeration
	 * of symbol indexes.  Consider this rela section unchanged.
	 */
	sec->status = SAME;
}

void kpatch_correlate_elfs(struct kpatch_elf *kelf1, struct kpatch_elf *kelf2)
{
	struct section *sec;
	int i;

	kpatch_correlate_sections(&kelf1->sections, &kelf2->sections);
	kpatch_correlate_symbols(&kelf1->symbols, &kelf2->symbols);

	/* at this point, sections are correlated, we can use sec->twin */
	for_each_section(i, sec, &kelf1->sections)
		if (is_rela_section(sec))
			kpatch_correlate_relas(sec);
}

void kpatch_compare_correlated_elements(struct kpatch_elf *kelf)
{
	struct section *sec;
	int i;

	/* tables are already correlated at this point */
	kpatch_compare_correlated_nonrela_sections(&kelf->sections);
	kpatch_compare_correlated_symbols(&kelf->symbols);

	for_each_section(i, sec, &kelf->sections)
		if (is_rela_section(sec) && sec->status == SAME)
			kpatch_set_rela_section_status(sec);
}

void kpatch_replace_sections_syms(struct kpatch_elf *kelf)
{
	struct section *sec;
	struct rela *rela;
	int i, j;

	for_each_section(i, sec, &kelf->sections) {
		if (!is_rela_section(sec))
			continue;

		for_each_rela(j, rela, &sec->relas) {
			if (rela->sym->type != STT_SECTION ||
			    !rela->sym->sec || !rela->sym->sec->sym)
				continue;

			log_debug("replacing %s with %s\n",
			       rela->sym->name, rela->sym->sec->sym->name);

			rela->sym = rela->sym->sec->sym;
		}
	}
}

void kpatch_dump_kelf(struct kpatch_elf *kelf)
{
	struct section *sec;
	struct symbol *sym;
	struct rela *rela;
	int i, j;

	if (loglevel > DEBUG)
		return;

	printf("\n=== Sections ===\n");
	for_each_section(i, sec, &kelf->sections) {
		printf("%02d %s (%s)", sec->index, sec->name, status_str(sec->status));
		if (is_rela_section(sec)) {
			printf(", base-> %s\n", sec->base->name);
			printf("rela section expansion\n");
			for_each_rela(j, rela, &sec->relas) {
				printf("sym %lu, offset %d, type %d, %s %s %d %s\n",
				       GELF_R_SYM(rela->rela.r_info),
				       rela->offset, rela->type,
				       rela->sym->name,
				       (rela->addend < 0)?"-":"+",
				       abs(rela->addend),
				       status_str(rela->status));
			}
		} else {
			if (sec->sym)
				printf(", sym-> %s", sec->sym->name);
			if (sec->secsym)
				printf(", secsym-> %s", sec->secsym->name);
			if (sec->rela)
				printf(", rela-> %s", sec->rela->name);
		}
		printf("\n");
	}

	printf("\n=== Symbols ===\n");
	for_each_symbol(i, sym, &kelf->symbols) {
		if (i == 0) /* ugh */
			continue;
		printf("sym %02d, type %d, bind %d, ndx %02d, name %s (%s)",
			sym->index, sym->type, sym->bind, sym->sym.st_shndx,
			sym->name, status_str(sym->status));
		if (sym->sec && (sym->type == STT_FUNC || sym->type == STT_OBJECT))
			printf(" -> %s", sym->sec->name);
		printf("\n");
	}
}

int kpatch_find_changed_functions(struct kpatch_elf *kelf)
{
	struct symbol *sym;
	int i, changed = 0;

	for_each_symbol(i, sym, &kelf->symbols) {
		if (sym->type != STT_FUNC)
			continue;
		if (sym->status == CHANGED) {
			changed = 1;
			printf("function %s has changed\n",sym->name);
		}
	}

	if (!changed)
		printf("no changes found\n");
			
	return changed;
}

#define inc_printf(fmt, ...) \
	log_debug("%*s" fmt, recurselevel, "", ##__VA_ARGS__);

void kpatch_include_symbol(struct symbol *sym, int recurselevel)
{
	struct rela *rela;
	struct section *sec;
	int i;

	inc_printf("start include_symbol(%s)\n", sym->name);
	sym->include = 1;
	inc_printf("symbol %s is included\n", sym->name);
	/*
	 * Check if sym is a non-local symbol (sym->sec is NULL) or
	 * if an unchanged local symbol.  This a base case for the
	 * inclusion recursion.
	 */
	if (!sym->sec || (sym->type != STT_SECTION && sym->status == SAME))
		goto out;
	sec = sym->sec;
	sec->include = 1;
	inc_printf("section %s is included\n", sec->name);
	if (sec->secsym == sym)
		goto out;
	if (sec->secsym) {
		sec->secsym->include = 1;
		inc_printf("section symbol %s is included\n", sec->secsym->name);
	}
	if (!sec->rela)
		goto out;
	sec->rela->include = 1;
	inc_printf("section %s is included\n", sec->rela->name);
	for_each_rela(i, rela, &sec->rela->relas) {
		if (rela->sym->include)
			continue;
		kpatch_include_symbol(rela->sym, recurselevel+1);
	}
out:
	inc_printf("end include_symbol(%s)\n", sym->name);
	return;
}

void kpatch_include_changed_functions(struct kpatch_elf *kelf)
{
	struct symbol *sym;
	int i;

	log_debug("\n=== Inclusion Tree ===\n");

	for_each_symbol(i, sym, &kelf->symbols) {
		if (sym->status == CHANGED &&
		    sym->type == STT_FUNC &&
		    !sym->include) {
			log_normal("changed function: %s\n", sym->name);
			kpatch_include_symbol(sym, 0);
		}

		if (sym->type == STT_FILE)
			sym->include = 1;
	}
}

int kpatch_copy_symbols(int startndx, struct kpatch_elf *src,
                        struct kpatch_elf *dst,
                        int (*select)(struct symbol *))
{
	struct symbol *srcsym, *dstsym;
	int i, index = startndx;

	for_each_symbol(i, srcsym, &src->symbols) {
		if (i == 0 || !srcsym->include)
			continue;

		if (select && !select(srcsym))
			continue;

		dstsym = &((struct symbol *)(dst->symbols.data))[index];
		*dstsym = *srcsym;
		dstsym->index = index;
		dstsym->twino = srcsym;
		srcsym->twino = dstsym;
		index++;

		if (srcsym->sec && srcsym->sec->twino)
			dstsym->sym.st_shndx = srcsym->sec->twino->index;

		srcsym->include = 0;
	}

	return index;
}

int is_file_sym(struct symbol *sym)
{
	return sym->type == STT_FILE;
}

int is_local_func_sym(struct symbol *sym)
{
	return sym->bind == STB_LOCAL && sym->type == STT_FUNC;
}

int is_local_sym(struct symbol *sym)
{
	return sym->bind == STB_LOCAL;
}

void kpatch_generate_output(struct kpatch_elf *kelf, struct kpatch_elf **kelfout)
{
	int sections_nr = 0, symbols_nr = 0, i, index;
	struct section *sec, *secout;
	struct symbol *sym;
	struct kpatch_elf *out;

	/* count output sections */
	for_each_section(i, sec, &kelf->sections) {
		/* include these sections even if they haven't changed */
		if (!strcmp(sec->name, ".shstrtab") ||
		     !strcmp(sec->name, ".strtab") ||
		     !strcmp(sec->name, ".symtab"))
			sec->include = 1;

		if (sec->include)
			sections_nr++;
	}

	log_debug("outputting %d sections\n",sections_nr);

	/* count output symbols */
	for_each_symbol(i, sym, &kelf->symbols) {
		if (i == 0 || sym->include)
			symbols_nr++;
	}

	log_debug("outputting %d symbols\n",symbols_nr);

	/* allocate output kelf */
	out = malloc(sizeof(*out));
	if (!out)
		ERROR("malloc");
	memset(out, 0, sizeof(*out));

	/* allocate tables */
	alloc_table(&out->sections, sizeof(struct section), sections_nr);
	alloc_table(&out->symbols, sizeof(struct symbol), symbols_nr);

	/* copy to output kelf sections, link to kelf, and reindex */
	index = 0;
	for_each_section(i, sec, &kelf->sections) {
		if (!sec->include)
			continue;

		secout = &((struct section *)(out->sections.data))[index];
		*secout = *sec;
		secout->index = ++index;
		secout->twino = sec;
		sec->twino = secout;
	}

	/*
	 * Search symbol table for local functions and objects whose sections
	 * are not included, and modify them to be non-local.
	 */
	for_each_symbol(i, sym, &kelf->symbols) {
		if (i == 0)
			continue;
		if ((sym->type == STT_OBJECT ||
		     sym->type == STT_FUNC) &&
		    !sym->sec->include) {
			sym->type = STT_NOTYPE;
			sym->bind = STB_GLOBAL;
			sym->sym.st_info = GELF_ST_INFO(STB_GLOBAL, STT_NOTYPE);
			sym->sym.st_shndx = SHN_UNDEF;
			sym->sym.st_size = 0;
		}
	}

	/*
	 * Copy functions to the output kelf and reindex.  Once the symbol is
	 * copied, its include field is set to zero so it isn't copied again
	 * by a subsequent kpatch_copy_symbols() call.
	 */
	/* start at 1 to skip over symbol 0 (all zeros) */
	index = 1;
	/* copy (LOCAL) FILE sym */
	index = kpatch_copy_symbols(index, kelf, out, is_file_sym);
	/* copy LOCAL FUNC syms */
	index = kpatch_copy_symbols(index, kelf, out, is_local_func_sym);
	/* copy all other LOCAL syms */
	index = kpatch_copy_symbols(index, kelf, out, is_local_sym);
	/* copy all other (GLOBAL) syms */
	index = kpatch_copy_symbols(index, kelf, out, NULL);

	*kelfout = out;
}

void kpatch_write_inventory_file(struct kpatch_elf *kelf, char *outfile)
{
	FILE *out;
	char outbuf[255];
	int i;
	struct section *sec;
	struct symbol *sym;

	if (snprintf(outbuf, 254, "%s.inventory", outfile) < 0)
		ERROR("snprintf");

	out = fopen(outbuf, "w");
	if (!out)
		ERROR("fopen");

	for_each_section(i, sec, &kelf->sections)
		fprintf(out, "section %s\n", sec->name);

	for_each_symbol(i, sym, &kelf->symbols) {
		if (i == 0)
			continue;
		fprintf(out, "symbol %s %d %d\n", sym->name, sym->type, sym->bind);
	}

	fclose(out);
}

void kpatch_create_rela_section(struct section *sec, int link)
{
	struct rela *rela;
	int i, symndx, type;
	char *buf;
	size_t size;

	/* create new rela data buffer */
	size = sec->sh.sh_size;
	buf = malloc(size);
	if (!buf)
		ERROR("malloc");
	memset(buf, 0, size);

	/* reindex and copy into buffer */
	for_each_rela(i, rela, &sec->relas) {
		if (!rela->sym || !rela->sym->twino)
			ERROR("expected rela symbol");
		symndx = rela->sym->twino->index;
		type = GELF_R_TYPE(rela->rela.r_info);
		rela->rela.r_info = GELF_R_INFO(symndx, type);

		memcpy(buf + (i * sec->sh.sh_entsize), &rela->rela,
		       sec->sh.sh_entsize);
	}

	sec->data->d_buf = buf;
	/* size is unchanged */

	sec->sh.sh_link = link;
	/* info is section index of text section that matches this rela */
	sec->sh.sh_info = sec->twino->base->twino->index;
}

void kpatch_create_rela_sections(struct kpatch_elf *kelf)
{
	struct section *sec;
	int i, link;

	link = find_section_by_name(&kelf->sections, ".symtab")->index;

	/* reindex rela symbols */
	for_each_section(i, sec, &kelf->sections)
		if (is_rela_section(sec))
			kpatch_create_rela_section(sec, link);
}

void print_strtab(char *buf, size_t size)
{
	int i;

	for (i = 0; i < size; i++) {
		if (buf[i] == 0)
			printf("\\0");
		else
			printf("%c",buf[i]);
	}
}

void kpatch_create_shstrtab(struct kpatch_elf *kelf)
{
	struct section *shstrtab, *sec;
	size_t size, offset, len;
	int i;
	char *buf;

	shstrtab = find_section_by_name(&kelf->sections, ".shstrtab");
	if (!shstrtab)
		ERROR("find_section_by_name");

	/* determine size of string table */
	size = 1; /* for initial NULL terminator */
	for_each_section(i, sec, &kelf->sections)
		size += strlen(sec->name) + 1; /* include NULL terminator */

	/* allocate data buffer */
	buf = malloc(size);
	if (!buf)
		ERROR("malloc");
	memset(buf, 0, size);

	/* populate string table and link with section header */
	offset = 1;
	for_each_section(i, sec, &kelf->sections) {
		len = strlen(sec->name) + 1;
		sec->sh.sh_name = offset;
		memcpy(buf + offset, sec->name, len);
		offset += len;
	}

	if (offset != size)
		ERROR("shstrtab size mismatch");

	shstrtab->data->d_buf = buf;
	shstrtab->data->d_size = size;

	if (loglevel <= DEBUG) {
		printf("shstrtab: ");
		print_strtab(buf, size);
		printf("\n");

		for_each_section(i, sec, &kelf->sections)
			printf("%s @ shstrtab offset %d\n",
			       sec->name, sec->sh.sh_name);
	}
}

void kpatch_create_strtab(struct kpatch_elf *kelf)
{
	struct section *strtab;
	struct symbol *sym;
	size_t size, offset, len;
	int i;
	char *buf;

	strtab = find_section_by_name(&kelf->sections, ".strtab");
	if (!strtab)
		ERROR("find_section_by_name");

	/* determine size of string table */
	size = 1; /* for initial NULL terminator */
	for_each_symbol(i, sym, &kelf->symbols) {
		if (i == 0 || sym->type == STT_SECTION)
			continue;
		size += strlen(sym->name) + 1; /* include NULL terminator */
	}

	/* allocate data buffer */
	buf = malloc(size);
	if (!buf)
		ERROR("malloc");
	memset(buf, 0, size);

	/* populate string table and link with section header */
	offset = 1;
	for_each_symbol(i, sym, &kelf->symbols) {
		if (i == 0)
			continue;
		if (sym->type == STT_SECTION) {
			sym->sym.st_name = 0;
			continue;
		}
		len = strlen(sym->name) + 1;
		sym->sym.st_name = offset;
		memcpy(buf + offset, sym->name, len);
		offset += len;
	}

	if (offset != size)
		ERROR("shstrtab size mismatch");

	strtab->data->d_buf = buf;
	strtab->data->d_size = size;

	if (loglevel <= DEBUG) {
		printf("strtab: ");
		print_strtab(buf, size);
		printf("\n");

		for_each_symbol(i, sym, &kelf->symbols)
			printf("%s @ strtab offset %d\n",
			       sym->name, sym->sym.st_name);
	}
}

void kpatch_create_symtab(struct kpatch_elf *kelf)
{
	struct section *symtab;
	struct symbol *sym;
	char *buf;
	size_t size;
	int i;

	symtab = find_section_by_name(&kelf->sections, ".symtab");
	if (!symtab)
		ERROR("find_section_by_name");

	/* create new symtab buffer */
	size = kelf->symbols.nr * symtab->sh.sh_entsize;
	buf = malloc(size);
	if (!buf)
		ERROR("malloc");
	memset(buf, 0, size);

	for_each_symbol(i, sym, &kelf->symbols) {
		memcpy(buf + (i * symtab->sh.sh_entsize), &sym->sym,
		       symtab->sh.sh_entsize);
	}

	symtab->data->d_buf = buf;
	symtab->data->d_size = size;

	symtab->sh.sh_link =
		find_section_by_name(&kelf->sections, ".strtab")->index;
	symtab->sh.sh_info =
		find_section_by_name(&kelf->sections, ".shstrtab")->index;
}

void kpatch_write_output_elf(struct kpatch_elf *kelf, Elf *elf, char *outfile)
{
	int fd, i;
	struct section *sec;
	Elf *elfout;
	GElf_Ehdr eh, ehout;
	Elf_Scn *scn;
	Elf_Data *data;
	GElf_Shdr sh;

	/* TODO make this argv */
	fd = creat(outfile, 0777);
	if (fd == -1)
		ERROR("creat");

	elfout = elf_begin(fd, ELF_C_WRITE, NULL);
	if (!elfout)
		ERROR("elf_begin");

	if (!gelf_newehdr(elfout, gelf_getclass(kelf->elf)))
		ERROR("gelf_newehdr");

	if (!gelf_getehdr(elfout, &ehout))
		ERROR("gelf_getehdr");

	if (!gelf_getehdr(elf, &eh))
		ERROR("gelf_getehdr");

	memset(&ehout, 0, sizeof(ehout));
	ehout.e_ident[EI_DATA] = eh.e_ident[EI_DATA];
	ehout.e_machine = eh.e_machine;
	ehout.e_type = eh.e_type;
	ehout.e_version = EV_CURRENT;
	ehout.e_shstrndx = find_section_by_name(&kelf->sections, ".shstrtab")->index;

	/* add changed sections */
	for_each_section(i, sec, &kelf->sections) {
		scn = elf_newscn(elfout);
		if (!scn)
			ERROR("elf_newscn");

		data = elf_newdata(scn);
		if (!data)
			ERROR("elf_newdata");

		*data = *sec->data;

		if(!gelf_getshdr(scn, &sh))
			ERROR("gelf_getshdr");

		sh = sec->sh;

		if (!elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY))
			ERROR("elf_flagdata");

		if (!gelf_update_shdr(scn, &sh))
			ERROR("gelf_update_shdr");	
	}

	if (!gelf_update_ehdr(elfout, &ehout))
		ERROR("gelf_update_ehdr");

	if (elf_update(elfout, ELF_C_WRITE) < 0) {
		printf("%s\n",elf_errmsg(-1));
		ERROR("elf_update");
	}
}

struct arguments {
	char *args[3];
	int debug;
	int inventory;
};

static char args_doc[] = "original.o patched.o output.o";

static struct argp_option options[] = {
	{"debug", 'd', 0, 0, "Show debug output" },
	{"inventory", 'i', 0, 0, "Create inventory file with list of sections and symbols" },
	{ 0 }
};

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	/* Get the input argument from argp_parse, which we
	   know is a pointer to our arguments structure. */
	struct arguments *arguments = state->input;

	switch (key)
	{
		case 'd':
			arguments->debug = 1;
			break;
		case 'i':
			arguments->inventory = 1;
			break;
		case ARGP_KEY_ARG:
			if (state->arg_num >= 3)
				/* Too many arguments. */
				argp_usage (state);
			arguments->args[state->arg_num] = arg;
			break;
		case ARGP_KEY_END:
			if (state->arg_num < 3)
				/* Not enough arguments. */
				argp_usage (state);
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { options, parse_opt, args_doc, 0 };

int main(int argc, char *argv[])
{
	struct kpatch_elf *kelf_base, *kelf_patched, *kelf_out;
	char *outfile;
	struct arguments arguments;

	arguments.debug = 0;
	arguments.inventory = 0;
	argp_parse (&argp, argc, argv, 0, 0, &arguments);
	if (arguments.debug)
		loglevel = DEBUG;

	elf_version(EV_CURRENT);

	kelf_base = kpatch_elf_open(arguments.args[0]);
	kelf_patched = kpatch_elf_open(arguments.args[1]);
	outfile = arguments.args[2];

	kpatch_compare_elf_headers(kelf_base->elf, kelf_patched->elf);
	kpatch_check_program_headers(kelf_base->elf);
	kpatch_check_program_headers(kelf_patched->elf);

	kpatch_correlate_elfs(kelf_base, kelf_patched);
	/*
	 * After this point, we don't care about kelf_base anymore.
	 * We access its sections via the twin pointers in the
	 * section, symbol, and rela lists of kelf_patched.
	 */
	kpatch_compare_correlated_elements(kelf_patched);

	/*
	 * Mangle the relas a little.  The compiler will sometimes
	 * use section symbols to reference local objects and functions
	 * rather than the object or function symbols themselves.
	 * We substitute the object/function symbols for the section
	 * symbol in this case so that the existing object/function
	 * in vmlinux can be linked to.
	 */
	kpatch_replace_sections_syms(kelf_patched);

	kpatch_include_changed_functions(kelf_patched);
	kpatch_dump_kelf(kelf_patched);

	/* Generate the output elf */
	kpatch_generate_output(kelf_patched, &kelf_out);
	kpatch_create_rela_sections(kelf_out);
	kpatch_create_shstrtab(kelf_out);
	kpatch_create_strtab(kelf_out);
	kpatch_create_symtab(kelf_out);
	kpatch_dump_kelf(kelf_out);

	if (arguments.inventory)
		kpatch_write_inventory_file(kelf_out, outfile);
	kpatch_write_output_elf(kelf_out, kelf_patched->elf, outfile);

	return 0;
}
