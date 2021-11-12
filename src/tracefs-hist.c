// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2021 VMware Inc, Steven Rostedt <rostedt@goodmis.org>
 *
 * Updates:
 * Copyright (C) 2021, VMware, Tzvetomir Stoyanov <tz.stoyanov@gmail.com>
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include "tracefs.h"
#include "tracefs-local.h"

#define HIST_FILE "hist"

#define ASCENDING ".ascending"
#define DESCENDING ".descending"

struct tracefs_hist {
	struct tep_handle	*tep;
	struct tep_event	*event;
	char			*system;
	char			*event_name;
	char			*name;
	char			**keys;
	char			**values;
	char			**sort;
	char			*filter;
	int			size;
	unsigned int		filter_parens;
	unsigned int		filter_state;
};

static void add_list(struct trace_seq *seq, const char *start,
		     char **list)
{
	int i;

	trace_seq_puts(seq, start);
	for (i = 0; list[i]; i++) {
		if (i)
			trace_seq_putc(seq, ',');
		trace_seq_puts(seq, list[i]);
	}
}

static void add_hist_commands(struct trace_seq *seq, struct tracefs_hist *hist,
			     enum tracefs_hist_command command)
{
	if (command == TRACEFS_HIST_CMD_DESTROY)
		trace_seq_putc(seq, '!');

	add_list(seq, "hist:keys=", hist->keys);

	if (hist->values)
		add_list(seq, ":vals=", hist->values);

	if (hist->sort)
		add_list(seq, ":sort=", hist->sort);

	if (hist->size)
		trace_seq_printf(seq, ":size=%d", hist->size);

	switch(command) {
	case TRACEFS_HIST_CMD_START: break;
	case TRACEFS_HIST_CMD_PAUSE: trace_seq_puts(seq, ":pause"); break;
	case TRACEFS_HIST_CMD_CONT: trace_seq_puts(seq, ":cont"); break;
	case TRACEFS_HIST_CMD_CLEAR: trace_seq_puts(seq, ":clear"); break;
	default: break;
	}

	if (hist->name)
		trace_seq_printf(seq, ":name=%s", hist->name);

	if (hist->filter)
		trace_seq_printf(seq, " if %s", hist->filter);

}

/*
 * trace_hist_show - show how to start the histogram
 * @seq: A trace_seq to store the commands to create
 * @hist: The histogram to write into the trigger file
 * @command: If not zero, can pause, continue or clear the histogram
 *
 * This shows the commands to create the histogram for an event
 * with the given fields.
 *
 * Returns 0 on succes -1 on error.
 */
int
tracefs_hist_show(struct trace_seq *seq, struct tracefs_instance *instance,
		  struct tracefs_hist *hist,
		  enum tracefs_hist_command command)
{
	const char *system = hist->system;
	const char *event = hist->event_name;
	char *path;

	if (!hist->keys) {
		errno = -EINVAL;
		return -1;
	}

	path = tracefs_event_get_file(instance, system, event, "trigger");
	if (!path)
		return -1;

	trace_seq_puts(seq, "echo '");

	add_hist_commands(seq, hist, command);

	trace_seq_printf(seq, "' > %s\n", path);

	tracefs_put_tracing_file(path);

	return 0;
}

/*
 * tracefs_hist_command - Create, start, pause, destroy a histogram for an event
 * @instance: The instance the histogram will be in (NULL for toplevel)
 * @hist: The histogram to write into the trigger file
 * @command: Command to perform on a histogram.
 *
 * Creates, pause, continue, clears, or destroys a histogram.
 *
 * Returns 0 on succes -1 on error.
 */
int tracefs_hist_command(struct tracefs_instance *instance,
			 struct tracefs_hist *hist,
			 enum tracefs_hist_command command)
{
	const char *system = hist->system;
	const char *event = hist->event_name;
	struct trace_seq seq;
	int ret;

	if (!tracefs_event_file_exists(instance, system, event, HIST_FILE))
		return -1;

	errno = -EINVAL;
	if (!hist->keys)
		return -1;

	trace_seq_init(&seq);

	add_hist_commands(&seq, hist, command);

	trace_seq_putc(&seq, '\n');
	trace_seq_terminate(&seq);

	ret = -1;
	if (seq.state == TRACE_SEQ__GOOD)
		ret = tracefs_event_file_append(instance, system, event,
						"trigger", seq.buffer);

	trace_seq_destroy(&seq);

	return ret < 0 ? -1 : 0;
}

/**
 * tracefs_hist_free - free a tracefs_hist element
 * @hist: The histogram to free
 */
void tracefs_hist_free(struct tracefs_hist *hist)
{
	if (!hist)
		return;

	tep_unref(hist->tep);
	free(hist->system);
	free(hist->event_name);
	free(hist->name);
	free(hist->filter);
	tracefs_list_free(hist->keys);
	tracefs_list_free(hist->values);
	tracefs_list_free(hist->sort);
	free(hist);
}

/**
 * tracefs_hist_alloc - Initialize a histogram
 * @tep: The tep handle that has the @system and @event.
 * @system: The system the histogram event is in.
 * @event_name: The name of the event that the histogram will be attached to.
 * @key: The primary key the histogram will use
 * @type: The format type of the key.
 *
 * Will initialize a histogram descriptor that will be attached to
 * the @system/@event with the given @key as the primary. This only
 * initializes the descriptor, it does not start the histogram
 * in the kernel.
 *
 * Returns an initialized histogram on success.
 * NULL on failure.
 */
struct tracefs_hist *
tracefs_hist_alloc(struct tep_handle *tep,
			const char *system, const char *event_name,
			const char *key, enum tracefs_hist_key_type type)
{
	struct tep_event *event;
	struct tracefs_hist *hist;
	int ret;

	if (!system || !event_name || !key)
		return NULL;

	event = tep_find_event_by_name(tep, system, event_name);
	if (!event)
		return NULL;

	hist = calloc(1, sizeof(*hist));
	if (!hist)
		return NULL;

	tep_ref(tep);
	hist->tep = tep;

	hist->event = event;
	hist->system = strdup(system);
	hist->event_name = strdup(event_name);

	ret = tracefs_hist_add_key(hist, key, type);

	if (!hist->system || !hist->event || ret < 0) {
		tracefs_hist_free(hist);
		return NULL;
	}


	return hist;
}

/**
 * tracefs_hist_add_key - add to a key to a histogram
 * @hist: The histogram to add the key to.
 * @key: The name of the key field.
 * @type: The type of the key format.
 *
 * This adds a secondary or tertiary key to the histogram.
 *
 * Returns 0 on success, -1 on error.
 */
int tracefs_hist_add_key(struct tracefs_hist *hist, const char *key,
			 enum tracefs_hist_key_type type)
{
	bool use_key = false;
	char *key_type = NULL;
	char **new_list;
	int ret = -1;

	switch (type) {
	case TRACEFS_HIST_KEY_NORMAL:
		use_key = true;
		ret = 0;
		break;
	case TRACEFS_HIST_KEY_HEX:
		ret = asprintf(&key_type, "%s.hex", key);
		break;
	case TRACEFS_HIST_KEY_SYM:
		ret = asprintf(&key_type, "%s.sym", key);
		break;
	case TRACEFS_HIST_KEY_SYM_OFFSET:
		ret = asprintf(&key_type, "%s.sym-offset", key);
		break;
	case TRACEFS_HIST_KEY_SYSCALL:
		ret = asprintf(&key_type, "%s.syscall", key);
		break;
	case TRACEFS_HIST_KEY_EXECNAME:
		ret = asprintf(&key_type, "%s.execname", key);
		break;
	case TRACEFS_HIST_KEY_LOG:
		ret = asprintf(&key_type, "%s.log2", key);
		break;
	case TRACEFS_HIST_KEY_USECS:
		ret = asprintf(&key_type, "%s.usecs", key);
		break;
	case TRACEFS_HIST_KEY_MAX:
		/* error */
		break;
	}

	if (ret < 0)
		return -1;

	new_list = tracefs_list_add(hist->keys, use_key ? key : key_type);
	free(key_type);
	if (!new_list)
		return -1;

	hist->keys = new_list;

	return 0;
}

/**
 * tracefs_hist_add_value - add to a value to a histogram
 * @hist: The histogram to add the value to.
 * @key: The name of the value field.
 *
 * This adds a value field to the histogram.
 *
 * Returns 0 on success, -1 on error.
 */
int tracefs_hist_add_value(struct tracefs_hist *hist, const char *value)
{
	char **new_list;

	new_list = tracefs_list_add(hist->values, value);
	if (!new_list)
		return -1;

	hist->values = new_list;

	return 0;
}

/**
 * tracefs_hist_add_name - name a histogram
 * @hist: The histogram to name.
 * @name: The name of the histogram.
 *
 * Adds a name to the histogram. Named histograms will share their
 * data with other events that have the same name as if it was
 * a single histogram.
 *
 * If the histogram already has a name, this will fail.
 *
 * Returns 0 on success, -1 on error.
 */
int tracefs_hist_add_name(struct tracefs_hist *hist, const char *name)
{
	if (hist->name)
		return -1;

	hist->name = strdup(name);

	return hist->name ? 0 : -1;
}

static char **
add_sort_key(struct tracefs_hist *hist, const char *sort_key, char **list)
{
	char **key_list = hist->keys;
	char **val_list = hist->values;
	int i;

	if (strcmp(sort_key, TRACEFS_HIST_HITCOUNT) == 0)
		goto out;

	for (i = 0; key_list[i]; i++) {
		if (strcmp(key_list[i], sort_key) == 0)
			break;
	}

	if (!key_list[i] && val_list) {
		for (i = 0; val_list[i]; i++) {
			if (strcmp(val_list[i], sort_key) == 0)
				break;
			if (!val_list[i])
				return NULL;
		}
	}


 out:
	return tracefs_list_add(list, sort_key);
}

/**
 * tracefs_hist_add_sort_key - add a key for sorting the histogram
 * @hist: The histogram to add the sort key to
 * @sort_key: The key to sort (and the strings after it)
 *  Last one must be NULL.
 *
 * Add a list of sort keys in the order of priority that the
 * keys would be sorted on output. Keys must be added first.
 *
 * Returns 0 on success, -1 on error.
 */
int tracefs_hist_add_sort_key(struct tracefs_hist *hist,
			      const char *sort_key, ...)
{
	char **list = NULL;
	char **tmp;
	va_list ap;

	if (!hist || !sort_key)
		return -1;

	tmp = add_sort_key(hist, sort_key, list);
	if (!tmp)
		goto fail;
	list = tmp;

	va_start(ap, sort_key);
	for (;;) {
		sort_key = va_arg(ap, const char *);
		if (!sort_key)
			break;
		tmp = add_sort_key(hist, sort_key, list);
		if (!tmp)
			goto fail;
		list = tmp;
	}
	va_end(ap);

	tracefs_list_free(hist->sort);
	hist->sort = list;

	return 0;
 fail:
	tracefs_list_free(list);
	return -1;
}

static int end_match(const char *sort_key, const char *ending)
{
	int key_len = strlen(sort_key);
	int end_len = strlen(ending);

	if (key_len <= end_len)
		return 0;

	sort_key += key_len - end_len;

	return strcmp(sort_key, ending) == 0 ? key_len - end_len : 0;
}

/**
 * tracefs_hist_sort_key_direction - set direction of a sort key
 * @hist: The histogram to modify.
 * @sort_str: The sort key to set the direction for
 * @dir: The direction to set the sort key to.
 *
 * Returns 0 on success, and -1 on error;
 */
int tracefs_hist_sort_key_direction(struct tracefs_hist *hist,
				    const char *sort_str,
				    enum tracefs_hist_sort_direction dir)
{
	char **sort = hist->sort;
	char *sort_key;
	char *direct;
	int match;
	int i;

	if (!sort)
		return -1;

	for (i = 0; sort[i]; i++) {
		if (strcmp(sort[i], sort_str) == 0)
			break;
	}
	if (!sort[i])
		return -1;

	sort_key = sort[i];

	switch (dir) {
	case TRACEFS_HIST_SORT_ASCENDING:
		direct = ASCENDING;
		break;
	case TRACEFS_HIST_SORT_DESCENDING:
		direct = DESCENDING;
		break;
	default:
		return -1;
	}

	match = end_match(sort_key, ASCENDING);
	if (match) {
		/* Already match? */
		if (dir == TRACEFS_HIST_SORT_ASCENDING)
			return 0;
	} else {
		match = end_match(sort_key, DESCENDING);
		/* Already match? */
		if (match && dir == TRACEFS_HIST_SORT_DESCENDING)
			return 0;
	}

	if (match)
		/* Clear the original text */
		sort_key[match] = '\0';

	sort_key = realloc(sort_key, strlen(sort_key) + strlen(direct) + 1);
	if (!sort_key) {
		/* Failed to alloc, may need to put back the match */
		sort_key = sort[i];
		if (match)
			sort_key[match] = '.';
		return -1;
	}

	strcat(sort_key, direct);
	sort[i] = sort_key;
	return 0;
}

int tracefs_hist_append_filter(struct tracefs_hist *hist,
			       enum tracefs_filter type,
			       const char *field,
			       enum tracefs_compare compare,
			       const char *val)
{
	return trace_append_filter(&hist->filter, &hist->filter_state,
				   &hist->filter_parens,
				   hist->event,
				   type, field, compare, val);
}

enum action_type {
	ACTION_NONE,
	ACTION_TRACE,
	ACTION_SNAPSHOT,
	ACTION_SAVE,
};

struct action {
	struct action			*next;
	enum action_type		type;
	enum tracefs_synth_handler	handler;
	char				*handle_field;
	char				*save;
};

/*
 * @name: name of the synthetic event
 * @start_system: system of the starting event
 * @start_event: the starting event
 * @end_system: system of the ending event
 * @end_event: the ending event
 * @actions: List of actions to take
 * @match_names: If a match set is to be a synthetic field, it has a name
 * @start_match: list of keys in the start event that matches end event
 * @end_match: list of keys in the end event that matches the start event
 * @compare_names: The synthetic field names of the compared fields
 * @start_compare: A list of compare fields in the start to compare to end
 * @end_compare: A list of compare fields in the end to compare to start
 * @compare_ops: The type of operations to perform between the start and end
 * @start_names: The fields in the start event to record
 * @end_names: The fields in the end event to record
 * @start_filters: The fields in the end event to record
 * @end_filters: The fields in the end event to record
 * @start_parens: Current parenthesis level for start event
 * @end_parens: Current parenthesis level for end event
 */
struct tracefs_synth {
	struct tep_handle	*tep;
	struct tep_event	*start_event;
	struct tep_event	*end_event;
	struct action		*actions;
	struct action		**next_action;
	char			*name;
	char			**synthetic_fields;
	char			**synthetic_args;
	char			**start_selection;
	char			**start_keys;
	char			**end_keys;
	char			**start_vars;
	char			**end_vars;
	char			*start_filter;
	char			*end_filter;
	unsigned int		start_parens;
	unsigned int		start_state;
	unsigned int		end_parens;
	unsigned int		end_state;
	int			*start_type;
	char			arg_name[16];
	int			arg_cnt;
};

static void action_free(struct action *action)
{
	free(action->handle_field);
	free(action->save);
	free(action);
}

/**
 * tracefs_synth_free - free the resources alloced to a synth
 * @synth: The tracefs_synth descriptor
 *
 * Frees the resources allocated for a @synth created with
 * tracefs_synth_init(). It does not touch the system. That is,
 * any synthetic event created, will not be destroyed by this
 * function.
 */
void tracefs_synth_free(struct tracefs_synth *synth)
{
	struct action *action;

	if (!synth)
		return;

	free(synth->name);
	tracefs_list_free(synth->synthetic_fields);
	tracefs_list_free(synth->synthetic_args);
	tracefs_list_free(synth->start_keys);
	tracefs_list_free(synth->end_keys);
	tracefs_list_free(synth->start_vars);
	tracefs_list_free(synth->end_vars);
	free(synth->start_filter);
	free(synth->end_filter);

	tep_unref(synth->tep);

	while ((action = synth->actions)) {
		synth->actions = action->next;
		action_free(action);
	}

	free(synth);
}

static bool verify_event_fields(struct tep_event *start_event,
				struct tep_event *end_event,
				const char *start_field_name,
				const char *end_field_name,
				const struct tep_format_field **ptr_start_field)
{
	const struct tep_format_field *start_field;
	const struct tep_format_field *end_field;

	if (!trace_verify_event_field(start_event, start_field_name,
				      &start_field))
		return false;

	if (end_event) {
		if (!trace_verify_event_field(end_event, end_field_name,
					      &end_field))
			return false;

		if (start_field->flags != end_field->flags ||
		    start_field->size != end_field->size) {
			errno = EBADE;
			return false;
		}
	}

	if (ptr_start_field)
		*ptr_start_field = start_field;

	return true;
}

__hidden char *append_string(char *str, const char *space, const char *add)
{
	char *new;
	int len;

	/* String must already be allocated */
	if (!str)
		return NULL;

	len = strlen(str) + strlen(add) + 2;
	if (space)
		len += strlen(space);

	new = realloc(str, len);
	if (!new) {
		free(str);
		return NULL;
	}
	str = new;

	if (space)
		strcat(str, space);
	strcat(str, add);

	return str;
}

static char *add_synth_field(const struct tep_format_field *field,
			     const char *name)
{
	const char *type;
	char size[64];
	char *str;
	bool sign;

	if (field->flags & TEP_FIELD_IS_ARRAY) {
		str = strdup("char");
		str = append_string(str, " ", name);
		str = append_string(str, NULL, "[");

		if (!(field->flags & TEP_FIELD_IS_DYNAMIC)) {
			snprintf(size, 64, "%d", field->size);
			str = append_string(str, NULL, size);
		}
		return append_string(str, NULL, "];");
	}

	sign = field->flags & TEP_FIELD_IS_SIGNED;

	switch (field->size) {
	case 1:
		if (!sign)
			type = "unsigned char";
		else
			type = "char";
		break;
	case 2:
		if (sign)
			type = "s16";
		else
			type = "u16";
		break;
	case 4:
		if (sign)
			type = "s32";
		else
			type = "u32";
		break;
	case 8:
		if (sign)
			type = "s64";
		else
			type = "u64";
		break;
	default:
		errno = EBADF;
		return NULL;
	}

	str = strdup(type);
	str = append_string(str, " ", name);
	return append_string(str, NULL, ";");
}

static int add_var(char ***list, const char *name, const char *var, bool is_var)
{
	char **new;
	char *assign;
	int ret;

	if (is_var)
		ret = asprintf(&assign, "%s=$%s", name, var);
	else
		ret = asprintf(&assign, "%s=%s", name, var);

	if (ret < 0)
		return -1;

	new = tracefs_list_add(*list, assign);
	free(assign);

	if (!new)
		return -1;
	*list = new;
	return 0;
}

__hidden struct tracefs_synth *
synth_init_from(struct tep_handle *tep, const char *start_system,
		const char *start_event_name)
{
	struct tep_event *start_event;
	struct tracefs_synth *synth;

	start_event = tep_find_event_by_name(tep, start_system,
					     start_event_name);
	if (!start_event) {
		errno = ENODEV;
		return NULL;
	}

	synth = calloc(1, sizeof(*synth));
	if (!synth)
		return NULL;

	synth->start_event = start_event;
	synth->next_action = &synth->actions;

	/* Hold onto a reference to this handler */
	tep_ref(tep);
	synth->tep = tep;

	return synth;
}

/**
 * tracefs_synth_init - create a new tracefs_synth instance
 * @tep: The tep handle that holds the events to work on
 * @name: The name of the synthetic event being created
 * @start_system: The name of the system of the start event (can be NULL)
 * @start_event_name: The name of the start event
 * @end_system: The name of the system of the end event (can be NULL)
 * @end_event_name: The name of the end event
 * @start_match_field: The name of the field in start event to match @end_match_field
 * @end_match_field: The name of the field in end event to match @start_match_field
 * @match_name: Name to call the fields that match (can be NULL)
 * 
 * Creates a tracefs_synth instance that has the minimum requirements to
 * create a synthetic event.
 *
 * @name is will be the name of the synthetic event that this can create.
 *
 * The start event is found with @start_system and @start_event_name. If
 * @start_system is NULL, then the first event with @start_event_name will
 * be used.
 *
 * The end event is found with @end_system and @end_event_name. If
 * @end_system is NULL, then the first event with @end_event_name will
 * be used.
 *
 * The @start_match_field is the field in the start event that will be used
 * to match the @end_match_field of the end event.
 *
 * If @match_name is given, then the field that matched the start and
 * end events will be passed an a field to the sythetic event with this
 * as the field name.
 *
 * Returns an allocated tracefs_synth descriptor on success and NULL
 * on error, with the following set in errno.
 *
 * ENOMEM - memory allocation failure.
 * ENIVAL - a parameter is passed as NULL that should not be
 * ENODEV - could not find an event or field
 * EBADE - The start and end fields are not compatible to match 
 * 
 * Note, this does not modify the system. That is, the synthetic
 * event on the system is not created. That needs to be done with
 * tracefs_synth_create().
 */
struct tracefs_synth *tracefs_synth_init(struct tep_handle *tep,
					 const char *name,
					 const char *start_system,
					 const char *start_event_name,
					 const char *end_system,
					 const char *end_event_name,
					 const char *start_match_field,
					 const char *end_match_field,
					 const char *match_name)
{
	struct tep_event *end_event;
	struct tracefs_synth *synth;
	int ret = 0;

	if (!tep || !name || !start_event_name || !end_event_name ||
	    !start_match_field || !end_match_field) {
		errno = EINVAL;
		return NULL;
	}

	synth = synth_init_from(tep, start_system, start_event_name);
	if (!synth)
		return NULL;

	end_event = tep_find_event_by_name(tep, end_system,
					   end_event_name);
	if (!end_event) {
		tep_unref(tep);
		errno = ENODEV;
		return NULL;
	}

	synth->end_event = end_event;

	synth->name = strdup(name);

	ret = tracefs_synth_add_match_field(synth, start_match_field,
					    end_match_field, match_name);

	if (!synth->name || !synth->start_keys || !synth->end_keys || ret) {
		tracefs_synth_free(synth);
		synth = NULL;
	}

	return synth;
}

static int add_synth_fields(struct tracefs_synth *synth,
			    const struct tep_format_field *field,
			    const char *name, const char *var)
{
	char **list;
	char *str;
	int ret;

	str = add_synth_field(field, name ? : field->name);
	if (!str)
		return -1;

	if (!name)
		name = var;

	list = tracefs_list_add(synth->synthetic_fields, str);
	free(str);
	if (!list)
		return -1;
	synth->synthetic_fields = list;

	ret = asprintf(&str, "$%s", name);
	if (ret < 0) {
		tracefs_list_pop(synth->synthetic_fields);
		return -1;
	}

	list = tracefs_list_add(synth->synthetic_args, str);
	free(str);
	if (!list) {
		tracefs_list_pop(synth->synthetic_fields);
		return -1;
	}

	synth->synthetic_args = list;

	return 0;
}

/**
 * tracefs_synth_add_match_field - add another key to match events
 * @synth: The tracefs_synth descriptor
 * @start_match_field: The field of the start event to match the end event
 * @end_match_field: The field of the end event to match the start event
 * @name: The name to show in the synthetic event (NULL is allowed)
 *
 * This will add another set of keys to use for a match between
 * the start event and the end event.
 *
 * Returns 0 on succes and -1 on error.
 * On error, errno is set to:
 * ENOMEM - memory allocation failure.
 * ENIVAL - a parameter is passed as NULL that should not be
 * ENODEV - could not find a field
 * EBADE - The start and end fields are not compatible to match 
 */
int tracefs_synth_add_match_field(struct tracefs_synth *synth,
				  const char *start_match_field,
				  const char *end_match_field,
				  const char *name)
{
	const struct tep_format_field *key_field;
	char **list;
	int ret;

	if (!synth || !start_match_field || !end_match_field) {
		errno = EINVAL;
		return -1;
	}

	if (!verify_event_fields(synth->start_event, synth->end_event,
				 start_match_field, end_match_field,
				 &key_field))
		return -1;

	list = tracefs_list_add(synth->start_keys, start_match_field);
	if (!list)
		return -1;

	synth->start_keys = list;

	list = tracefs_list_add(synth->end_keys, end_match_field);
	if (!list) {
		tracefs_list_pop(synth->start_keys);
		return -1;
	}
	synth->end_keys = list;

	if (!name)
		return 0;

	ret = add_var(&synth->end_vars, name, end_match_field, false);

	if (ret < 0)
		goto pop_lists;

	ret = add_synth_fields(synth, key_field, name, NULL);
	if (ret < 0)
		goto pop_lists;

	return 0;

 pop_lists:
	tracefs_list_pop(synth->start_keys);
	tracefs_list_pop(synth->end_keys);
	return -1;
}

static unsigned int make_rand(void)
{
	struct timeval tv;
	unsigned long seed;

	gettimeofday(&tv, NULL);
	seed = (tv.tv_sec + tv.tv_usec) + syscall(SYS_gettid);

	/* taken from the rand(3) man page */
	seed = seed * 1103515245 + 12345;
	return((unsigned)(seed/65536) % 32768);
}

static char *new_arg(struct tracefs_synth *synth)
{
	int cnt = synth->arg_cnt + 1;
	char *arg;
	int ret;

	/* Create a unique argument name */
	if (!synth->arg_name[0]) {
		/* make_rand() returns at most 32768 (total 13 bytes in use) */
		sprintf(synth->arg_name, "__arg_%u_", make_rand());
	}
	ret = asprintf(&arg, "%s%d", synth->arg_name, cnt);
	if (ret < 0)
		return NULL;

	synth->arg_cnt = cnt;
	return arg;
}

/**
 * tracefs_synth_add_compare_field - add a comparison between start and end
 * @synth: The tracefs_synth descriptor
 * @start_compare_field: The field of the start event to compare to the end
 * @end_compare_field: The field of the end event to compare to the start
 * @calc - How to go about the comparing the fields.
 * @name: The name to show in the synthetic event (must NOT be NULL)
 *
 * This will add a way to compare two different fields between the
 * start end end events.
 *
 * The comparing between events is decided by @calc:
 *    TRACEFS_SYNTH_DELTA_END       - name = end - start
 *    TRACEFS_SYNTH_DELTA_START     - name = start - end
 *    TRACEFS_SYNTH_ADD             - name = end + start
 *
 * Returns 0 on succes and -1 on error.
 * On error, errno is set to:
 * ENOMEM - memory allocation failure.
 * ENIVAL - a parameter is passed as NULL that should not be
 * ENODEV - could not find a field
 * EBADE - The start and end fields are not compatible to compare
 */
int tracefs_synth_add_compare_field(struct tracefs_synth *synth,
				    const char *start_compare_field,
				    const char *end_compare_field,
				    enum tracefs_synth_calc calc,
				    const char *name)
{
	const struct tep_format_field *start_field;
	char *start_arg;
	char *compare;
	int ret;

	/* Compare fields require a name */
	if (!name || !start_compare_field || !end_compare_field) {
		errno = -EINVAL;
		return -1;
	}

	if (!verify_event_fields(synth->start_event, synth->end_event,
				 start_compare_field, end_compare_field,
				 &start_field))
		return -1;

	/* Calculations are not allowed on string */
	if (start_field->flags & (TEP_FIELD_IS_ARRAY |
				  TEP_FIELD_IS_DYNAMIC)) {
		errno = -EINVAL;
		return -1;
	}

	start_arg = new_arg(synth);
	if (!start_arg)
		return -1;

	ret = add_var(&synth->start_vars, start_arg, start_compare_field, false);
	if (ret < 0) {
		free(start_arg);
		return -1;
	}

	ret = -1;
	switch (calc) {
	case TRACEFS_SYNTH_DELTA_END:
		ret = asprintf(&compare, "%s-$%s", end_compare_field,
			       start_arg);
		break;
	case TRACEFS_SYNTH_DELTA_START:
		ret = asprintf(&compare, "$%s-%s", start_arg,
			       end_compare_field);
		break;
	case TRACEFS_SYNTH_ADD:
		ret = asprintf(&compare, "%s+$%s", end_compare_field,
			       start_arg);
		break;
	}
	free(start_arg);
	if (ret < 0)
		return -1;

	ret = add_var(&synth->end_vars, name, compare, false);
	if (ret < 0)
		goto out_free;

	ret = add_synth_fields(synth, start_field, name, NULL);
	if (ret < 0)
		goto out_free;

 out_free:
	free(compare);

	return ret ? -1 : 0;
}

__hidden int synth_add_start_field(struct tracefs_synth *synth,
				   const char *start_field,
				   const char *name,
				   enum tracefs_hist_key_type type)
{
	const struct tep_format_field *field;
	char *start_arg;
	char **tmp;
	int *types;
	int len;
	int ret;

	if (!synth || !start_field) {
		errno = EINVAL;
		return -1;
	}

	if (!name)
		name = start_field;

	if (!trace_verify_event_field(synth->start_event, start_field, &field))
		return -1;

	start_arg = new_arg(synth);
	if (!start_arg)
		return -1;

	ret = add_var(&synth->start_vars, start_arg, start_field, false);
	if (ret)
		goto out_free;

	ret = add_var(&synth->end_vars, name, start_arg, true);
	if (ret)
		goto out_free;

	ret = add_synth_fields(synth, field, name, NULL);
	if (ret)
		goto out_free;

	tmp = tracefs_list_add(synth->start_selection, start_field);
	if (!tmp) {
		ret = -1;
		goto out_free;
	}
	synth->start_selection = tmp;

	len = tracefs_list_size(tmp);
	if (len <= 0) { /* ?? */
		ret = -1;
		goto out_free;
	}

	types = realloc(synth->start_type, sizeof(*types) * len);
	if (!types) {
		ret = -1;
		goto out_free;
	}
	synth->start_type = types;
	synth->start_type[len - 1] = type;

 out_free:
	free(start_arg);
	return ret;
}

/**
 * tracefs_synth_add_start_field - add a start field to save
 * @synth: The tracefs_synth descriptor
 * @start_field: The field of the start event to save
 * @name: The name to show in the synthetic event (if NULL @start_field is used)
 *
 * This adds a field named by @start_field of the start event to
 * record in the synthetic event.
 *
 * Returns 0 on succes and -1 on error.
 * On error, errno is set to:
 * ENOMEM - memory allocation failure.
 * ENIVAL - a parameter is passed as NULL that should not be
 * ENODEV - could not find a field
 */
int tracefs_synth_add_start_field(struct tracefs_synth *synth,
				  const char *start_field,
				  const char *name)
{
	return synth_add_start_field(synth, start_field, name, 0);
}

/**
 * tracefs_synth_add_end_field - add a end field to save
 * @synth: The tracefs_synth descriptor
 * @end_field: The field of the end event to save
 * @name: The name to show in the synthetic event (if NULL @end_field is used)
 *
 * This adds a field named by @end_field of the start event to
 * record in the synthetic event.
 *
 * Returns 0 on succes and -1 on error.
 * On error, errno is set to:
 * ENOMEM - memory allocation failure.
 * ENIVAL - a parameter is passed as NULL that should not be
 * ENODEV - could not find a field
 */
int tracefs_synth_add_end_field(struct tracefs_synth *synth,
				const char *end_field,
				const char *name)
{
	const struct tep_format_field *field;
	char *tmp_var = NULL;
	int ret;

	if (!synth || !end_field) {
		errno = EINVAL;
		return -1;
	}

	if (!name)
		tmp_var = new_arg(synth);

	if (!trace_verify_event_field(synth->end_event, end_field, &field))
		return -1;

	ret = add_var(&synth->end_vars, name ? : tmp_var, end_field, false);
	if (ret)
		goto out;

	ret = add_synth_fields(synth, field, name, tmp_var);
	free(tmp_var);
 out:
	return ret;
}

/**
 * tracefs_synth_append_start_filter - create or append a filter
 * @synth: The tracefs_synth descriptor
 * @type: The type of element to add to the filter
 * @field: For @type == TRACEFS_FILTER_COMPARE, the field to compare
 * @compare: For @type == TRACEFS_FILTER_COMPARE, how to compare @field to @val
 * @val: For @type == TRACEFS_FILTER_COMPARE, what value @field is to be
 *
 * This will put together a filter string for the starting event
 * of @synth. It check to make sure that what is added is correct compared
 * to the filter that is already built.
 *
 * @type can be:
 *     TRACEFS_FILTER_COMPARE:        See below
 *     TRACEFS_FILTER_AND:            Append "&&" to the filter
 *     TRACEFS_FILTER_OR:             Append "||" to the filter
 *     TRACEFS_FILTER_NOT:            Append "!" to the filter
 *     TRACEFS_FILTER_OPEN_PAREN:     Append "(" to the filter
 *     TRACEFS_FILTER_CLOSE_PAREN:    Append ")" to the filter
 *
 * For all types except TRACEFS_FILTER_COMPARE, the @field, @compare,
 * and @val are ignored.
 *
 * For @type == TRACEFS_FILTER_COMPARE.
 *
 *  @field is the name of the field for the start event to compare.
 *         If it is not a field for the start event, this return an
 *         error.
 *
 *  @compare can be one of:
 *     TRACEFS_COMPARE_EQ:       Test @field == @val
 *     TRACEFS_COMPARE_NE:       Test @field != @val
 *     TRACEFS_COMPARE_GT:       Test @field > @val
 *     TRACEFS_COMPARE_GE:       Test @field >= @val
 *     TRACEFS_COMPARE_LT:       Test @field < @val
 *     TRACEFS_COMPARE_LE:       Test @field <= @val
 *     TRACEFS_COMPARE_RE:       Test @field ~ @val
 *     TRACEFS_COMPARE_AND:      Test @field & @val
 *
 * If the @field is of type string, and @compare is not
 *   TRACEFS_COMPARE_EQ, TRACEFS_COMPARE_NE or TRACEFS_COMPARE_RE,
 *   then this will return an error.
 *
 * Various other checks are made, for instance, if more CLOSE_PARENs
 * are added than existing OPEN_PARENs. Or if AND is added after an
 * OPEN_PAREN or another AND or an OR or a NOT.
 *
 * Returns 0 on success and -1 on failure.
 */
int tracefs_synth_append_start_filter(struct tracefs_synth *synth,
				      enum tracefs_filter type,
				      const char *field,
				      enum tracefs_compare compare,
				      const char *val)
{
	return trace_append_filter(&synth->start_filter, &synth->start_state,
				   &synth->start_parens,
				   synth->start_event,
				   type, field, compare, val);
}

/**
 * tracefs_synth_append_end_filter - create or append a filter
 * @synth: The tracefs_synth descriptor
 * @type: The type of element to add to the filter
 * @field: For @type == TRACEFS_FILTER_COMPARE, the field to compare
 * @compare: For @type == TRACEFS_FILTER_COMPARE, how to compare @field to @val
 * @val: For @type == TRACEFS_FILTER_COMPARE, what value @field is to be
 *
 * Performs the same thing as tracefs_synth_append_start_filter() but
 * for the @synth end event.
 */
int tracefs_synth_append_end_filter(struct tracefs_synth *synth,
				    enum tracefs_filter type,
				    const char *field,
				    enum tracefs_compare compare,
				    const char *val)
{
	return trace_append_filter(&synth->end_filter, &synth->end_state,
				   &synth->end_parens,
				   synth->end_event,
				   type, field, compare, val);
}

static int test_max_var(struct tracefs_synth *synth, const char *var)
{
	char **vars = synth->end_vars;
	char *p;
	int len;
	int i;

	len = strlen(var);

	/* Make sure the var is defined for the end event */
	for (i = 0; vars[i]; i++) {
		p = strchr(vars[i], '=');
		if (!p)
			continue;
		if (p - vars[i] != len)
			continue;
		if (!strncmp(var, vars[i], len))
			return 0;
	}
	errno = ENODEV;
	return -1;
}

static struct action *create_action(enum tracefs_synth_handler type,
				    struct tracefs_synth *synth,
				    const char *var)
{
	struct action *action;
	int ret;

	switch (type) {
	case TRACEFS_SYNTH_HANDLE_MAX:
	case TRACEFS_SYNTH_HANDLE_CHANGE:
		ret = test_max_var(synth, var);
		if (ret < 0)
			return NULL;
		break;
	default:
		break;
	}

	action = calloc(1, sizeof(*action));
	if (!action)
		return NULL;

	if (var) {
		ret = asprintf(&action->handle_field, "$%s", var);
		if (!action->handle_field) {
			free(action);
			return NULL;
		}
	}
	return action;
}

static void add_action(struct tracefs_synth *synth, struct action *action)
{
	*synth->next_action = action;
	synth->next_action = &action->next;
}

/**
 * tracefs_synth_trace - Execute the trace option
 * @synth: The tracefs_synth descriptor
 * @type: The type of handler to attach the trace action with
 * @field: The field for handlers onmax and onchange (ignored otherwise)
 *
 * Add the action 'trace' for handlers onmatch, onmax and onchange.
 *
 * Returns 0 on succes, -1 on error.
 */
int tracefs_synth_trace(struct tracefs_synth *synth,
			enum tracefs_synth_handler type, const char *field)
{
	struct action *action;

	if (!synth || (!field && (type != TRACEFS_SYNTH_HANDLE_MATCH))) {
		errno = EINVAL;
		return -1;
	}

	action = create_action(type, synth, field);
	if (!action)
		return -1;

	action->type = ACTION_TRACE;
	action->handler = type;
	add_action(synth, action);
	return 0;
}

/**
 * tracefs_synth_snapshot - create a snapshot command to the histogram
 * @synth: The tracefs_synth descriptor
 * @type: The type of handler to attach the snapshot action with
 * @field: The field for handlers onmax and onchange
 *
 * Add the action to do a snapshot for handlers onmax and onchange.
 *
 * Returns 0 on succes, -1 on error.
 */
int tracefs_synth_snapshot(struct tracefs_synth *synth,
			   enum tracefs_synth_handler type, const char *field)
{
	struct action *action;

	if (!synth || !field || (type == TRACEFS_SYNTH_HANDLE_MATCH)) {
		errno = EINVAL;
		return -1;
	}

	action = create_action(type, synth, field);
	if (!action)
		return -1;

	action->type = ACTION_SNAPSHOT;
	action->handler = type;
	add_action(synth, action);
	return 0;
}

/**
 * tracefs_synth_save - create a save command to the histogram
 * @synth: The tracefs_synth descriptor
 * @type: The type of handler to attach the save action
 * @max_field: The field for handlers onmax and onchange
 * @fields: The fields to save for the end variable
 *
 * Add the action to save fields for handlers onmax and onchange.
 *
 * Returns 0 on succes, -1 on error.
 */
int tracefs_synth_save(struct tracefs_synth *synth,
		       enum tracefs_synth_handler type, const char *max_field,
		       char **fields)
{
	struct action *action;
	char *save;
	int i;

	if (!synth || !max_field || (type == TRACEFS_SYNTH_HANDLE_MATCH)) {
		errno = EINVAL;
		return -1;
	}

	action = create_action(type, synth, max_field);
	if (!action)
		return -1;

	action->type = ACTION_SAVE;
	action->handler = type;
	*synth->next_action = action;
	synth->next_action = &action->next;

	save = strdup(".save(");
	if (!save)
		goto error;

	for (i = 0; fields[i]; i++) {
		char *delim = i ? "," : NULL;

		if (!trace_verify_event_field(synth->end_event, fields[i], NULL))
			goto error;
		save = append_string(save, delim, fields[i]);
	}
	save = append_string(save, NULL, ")");
	if (!save)
		goto error;

	action->save = save;
	add_action(synth, action);
	return 0;
 error:
	action_free(action);
	free(save);
	return -1;
	return 0;
}

static char *create_synthetic_event(struct tracefs_synth *synth)
{
	char *synthetic_event;
	const char *field;
	int i;

	synthetic_event = strdup(synth->name);
	if (!synthetic_event)
		return NULL;

	for (i = 0; synth->synthetic_fields && synth->synthetic_fields[i]; i++) {
		field = synth->synthetic_fields[i];
		synthetic_event = append_string(synthetic_event, " ", field);
	}

	return synthetic_event;
}

static int remove_synthetic(const char *synthetic)
{
	char *str;
	int ret;

	ret = asprintf(&str, "!%s", synthetic);
	if (ret < 0)
		return -1;

	ret = tracefs_instance_file_append(NULL, "synthetic_events", str);
	free(str);
	return ret < 0 ? -1 : 0;
}

static int remove_hist(struct tracefs_instance *instance,
		       struct tep_event *event, const char *hist)
{
	char *str;
	int ret;

	ret = asprintf(&str, "!%s", hist);
	if (ret < 0)
		return -1;

	ret = tracefs_event_file_append(instance, event->system, event->name,
				  "trigger", str);
	free(str);
	return ret < 0 ? -1 : 0;
}

static char *create_hist(char **keys, char **vars)
{
	char *hist = strdup("hist:keys=");
	char *name;
	int i;

	if (!hist)
		return NULL;

	for (i = 0; keys[i]; i++) {
		name = keys[i];
		if (i)
			hist = append_string(hist, NULL, ",");
		hist = append_string(hist, NULL, name);
	}

	if (!vars)
		return hist;

	hist = append_string(hist, NULL, ":");

	for (i = 0; vars[i]; i++) {
		name = vars[i];
		if (i)
			hist = append_string(hist, NULL, ",");
		hist = append_string(hist, NULL, name);
	}

	return hist;
}

static char *create_onmatch(char *hist, struct tracefs_synth *synth)
{
	hist = append_string(hist, NULL, ":onmatch(");
	hist = append_string(hist, NULL, synth->start_event->system);
	hist = append_string(hist, NULL, ".");
	hist = append_string(hist, NULL, synth->start_event->name);
	return append_string(hist, NULL, ")");
}

static char *create_trace(char *hist, struct tracefs_synth *synth)
{
	char *name;
	int i;

	hist = append_string(hist, NULL, ".trace(");
	hist = append_string(hist, NULL, synth->name);

	for (i = 0; synth->synthetic_args && synth->synthetic_args[i]; i++) {
		name = synth->synthetic_args[i];

		hist = append_string(hist, NULL, ",");
		hist = append_string(hist, NULL, name);
	}

	return append_string(hist, NULL, ")");
}

static char *create_max(char *hist, struct tracefs_synth *synth, char *field)
{
	hist = append_string(hist, NULL, ":onmax(");
	hist = append_string(hist, NULL, field);
	return append_string(hist, NULL, ")");
}

static char *create_change(char *hist, struct tracefs_synth *synth, char *field)
{
	hist = append_string(hist, NULL, ":onchange(");
	hist = append_string(hist, NULL, field);
	return append_string(hist, NULL, ")");
}

static char *create_actions(char *hist, struct tracefs_synth *synth)
{
	struct action *action;

	if (!synth->actions) {
		/* Default is "onmatch" and "trace" */
		hist = create_onmatch(hist, synth);
		return create_trace(hist, synth);
	}

	for (action = synth->actions; action; action = action->next) {
		switch (action->handler) {
		case TRACEFS_SYNTH_HANDLE_MATCH:
			hist = create_onmatch(hist, synth);
			break;
		case TRACEFS_SYNTH_HANDLE_MAX:
			hist = create_max(hist, synth, action->handle_field);
			break;
		case TRACEFS_SYNTH_HANDLE_CHANGE:
			hist = create_change(hist, synth, action->handle_field);
			break;
		default:
			continue;
		}
		switch (action->type) {
		case ACTION_TRACE:
			hist = create_trace(hist, synth);
			break;
		case ACTION_SNAPSHOT:
			hist = append_string(hist, NULL, ".snapshot()");
			break;
		case ACTION_SAVE:
			hist = append_string(hist, NULL, action->save);
			break;
		default:
			continue;
		}
	}
	return hist;
}

static char *create_end_hist(struct tracefs_synth *synth)
{
	char *end_hist;

	end_hist = create_hist(synth->end_keys, synth->end_vars);
	return create_actions(end_hist, synth);
}

static char *append_filter(char *hist, char *filter, unsigned int parens)
{
	int i;

	if (!filter)
		return hist;

	hist = append_string(hist, NULL, " if ");
	hist = append_string(hist, NULL, filter);
	for (i = 0; i < parens; i++)
		hist = append_string(hist, NULL, ")");
	return hist;
}

static int verify_state(struct tracefs_synth *synth)
{
	if (trace_test_state(synth->start_state) < 0 ||
	    trace_test_state(synth->end_state) < 0)
		return -1;
	return 0;
}

/**
 * tracefs_synth_complete - tell if the tracefs_synth is complete or not
 * @synth: The synthetic event to get the start hist from.
 *
 * Retruns true if the synthetic event @synth has both a start and
 * end event (ie. a synthetic event, or just a histogram), and
 * false otherwise.
 */
bool tracefs_synth_complete(struct tracefs_synth *synth)
{
	return synth && synth->start_event && synth->end_event;
}

/**
 * tracefs_synth_get_start_hist - Return the histogram of the start event
 * @synth: The synthetic event to get the start hist from.
 *
 * On success, returns a tracefs_hist descriptor that holds the
 * histogram information of the start_event of the synthetic event
 * structure. Returns NULL on failure.
 */
struct tracefs_hist *
tracefs_synth_get_start_hist(struct tracefs_synth *synth)
{
	struct tracefs_hist *hist = NULL;
	struct tep_handle *tep;
	const char *system;
	const char *event;
	const char *key;
	char **keys;
	int *types;
	int ret;
	int i;

	if (!synth) {
		errno = EINVAL;
		return NULL;
	}

	system = synth->start_event->system;
	event = synth->start_event->name;
	types = synth->start_type;
	keys = synth->start_keys;
	tep = synth->tep;

	if (!keys)
		keys = synth->start_selection;

	if (!keys)
		return NULL;

	for (i = 0; keys[i]; i++) {
		int type = types ? types[i] : 0;

		if (type == HIST_COUNTER_TYPE)
			continue;

		key = keys[i];

		if (i) {
			ret = tracefs_hist_add_key(hist, key, type);
			if (ret < 0) {
				tracefs_hist_free(hist);
				return NULL;
			}
		} else {
			hist = tracefs_hist_alloc(tep, system, event,
						  key, type);
			if (!hist)
				return NULL;
		}
	}

	if (!hist)
		return NULL;

	for (i = 0; keys[i]; i++) {
		int type = types ? types[i] : 0;

		if (type != HIST_COUNTER_TYPE)
			continue;

		key = keys[i];

		ret = tracefs_hist_add_value(hist, key);
		if (ret < 0) {
			tracefs_hist_free(hist);
			return NULL;
		}
	}

	if (synth->start_filter) {
		hist->filter = strdup(synth->start_filter);
		if (!hist->filter) {
			tracefs_hist_free(hist);
			return NULL;
		}
	}

	return hist;
}

/**
 * tracefs_synth_create - creates the synthetic event on the system
 * @instance: The instance to modify the start and end events
 * @synth: The tracefs_synth descriptor
 *
 * This creates the synthetic events. The @instance is used for writing
 * the triggers into the start and end events.
 *
 * Returns 0 on succes and -1 on error.
 * On error, errno is set to:
 * ENOMEM - memory allocation failure.
 * ENIVAL - a parameter is passed as NULL that should not be or a problem
 *   writing into the system.
 */
int tracefs_synth_create(struct tracefs_instance *instance,
			 struct tracefs_synth *synth)
{
	char *synthetic_event;
	char *start_hist = NULL;
	char *end_hist = NULL;
	int ret;

	if (!synth) {
		errno = EINVAL;
		return -1;
	}

	if (!synth->name || !synth->end_event) {
		errno = EUNATCH;
		return -1;
	}

	if (verify_state(synth) < 0)
		return -1;

	synthetic_event = create_synthetic_event(synth);
	if (!synthetic_event)
		return -1;

	ret = tracefs_instance_file_append(NULL, "synthetic_events",
					   synthetic_event);
	if (ret < 0)
		goto free_synthetic;

	start_hist = create_hist(synth->start_keys, synth->start_vars);
	start_hist = append_filter(start_hist, synth->start_filter,
				   synth->start_parens);
	if (!start_hist)
		goto remove_synthetic;

	end_hist = create_end_hist(synth);
	end_hist = append_filter(end_hist, synth->end_filter,
				   synth->end_parens);
	if (!end_hist)
		goto remove_synthetic;

	ret = tracefs_event_file_append(instance, synth->start_event->system,
					synth->start_event->name,
					"trigger", start_hist);
	if (ret < 0)
		goto remove_synthetic;

	ret = tracefs_event_file_append(instance, synth->end_event->system,
					synth->end_event->name,
					"trigger", end_hist);
	if (ret < 0)
		goto remove_start_hist;

	free(start_hist);
	free(end_hist);

	return 0;

 remove_start_hist:
	remove_hist(instance, synth->start_event, start_hist);
 remove_synthetic:
	free(end_hist);
	free(start_hist);
	remove_synthetic(synthetic_event);
 free_synthetic:
	free(synthetic_event);
	return -1;
}

/**
 * tracefs_synth_destroy - delete the synthetic event from the system
 * @instance: The instance to modify the start and end events
 * @synth: The tracefs_synth descriptor
 *
 * This will destroy a synthetic event created by tracefs_synth_create()
 * with the same @instance and @synth.
 *
 * It will attempt to disable the synthetic event, but if other instances
 * have it active, it is likely to fail, which will likely fail on
 * all other parts of tearing down the synthetic event.
 *
 * Returns 0 on succes and -1 on error.
 * On error, errno is set to:
 * ENOMEM - memory allocation failure.
 * ENIVAL - a parameter is passed as NULL that should not be or a problem
 *   writing into the system.
 */
int tracefs_synth_destroy(struct tracefs_instance *instance,
			  struct tracefs_synth *synth)
{
	char *synthetic_event;
	char *hist;
	int ret;

	if (!synth) {
		errno = EINVAL;
		return -1;
	}

	if (!synth->name || !synth->end_event) {
		errno = EUNATCH;
		return -1;
	}

	/* Try to disable the event if possible */
	tracefs_event_disable(instance, "synthetic", synth->name);

	hist = create_end_hist(synth);
	hist = append_filter(hist, synth->end_filter,
			     synth->end_parens);
	if (!hist)
		return -1;
	ret = remove_hist(instance, synth->end_event, hist);
	free(hist);

	hist = create_hist(synth->start_keys, synth->start_vars);
	hist = append_filter(hist, synth->start_filter,
			     synth->start_parens);
	if (!hist)
		return -1;

	ret = remove_hist(instance, synth->start_event, hist);
	free(hist);

	synthetic_event = create_synthetic_event(synth);
	if (!synthetic_event)
		return -1;

	ret = remove_synthetic(synthetic_event);

	return ret ? -1 : 0;
}

/**
 * tracefs_synth_show - show the command lines to create the synthetic event
 * @seq: The trace_seq to store the command lines in
 * @instance: The instance to modify the start and end events
 * @synth: The tracefs_synth descriptor
 *
 * This will list the "echo" commands that are equivalent to what would
 * be executed by the tracefs_synth_create() command.
 *
 * Returns 0 on succes and -1 on error.
 * On error, errno is set to:
 * ENOMEM - memory allocation failure.
 */
int tracefs_synth_show(struct trace_seq *seq,
		       struct tracefs_instance *instance,
		       struct tracefs_synth *synth)
{
	char *synthetic_event = NULL;
	char *hist = NULL;
	char *path;
	int ret = -1;

	if (!synth) {
		errno = EINVAL;
		return -1;
	}

	if (!synth->name || !synth->end_event) {
		errno = EUNATCH;
		return -1;
	}

	synthetic_event = create_synthetic_event(synth);
	if (!synthetic_event)
		return -1;

	path = trace_find_tracing_dir();
	if (!path)
		goto out_free;

	trace_seq_printf(seq, "echo '%s' > %s/synthetic_events\n",
			 synthetic_event, path);

	tracefs_put_tracing_file(path);
	path = tracefs_instance_get_dir(instance);

	hist = create_hist(synth->start_keys, synth->start_vars);
	hist = append_filter(hist, synth->start_filter,
			     synth->start_parens);
	if (!hist)
		goto out_free;

	trace_seq_printf(seq, "echo '%s' > %s/events/%s/%s/trigger\n",
			 hist, path, synth->start_event->system,
			 synth->start_event->name);
	free(hist);
	hist = create_end_hist(synth);
	hist = append_filter(hist, synth->end_filter,
			     synth->end_parens);
	if (!hist)
		goto out_free;

	trace_seq_printf(seq, "echo '%s' > %s/events/%s/%s/trigger\n",
			 hist, path, synth->end_event->system,
			 synth->end_event->name);

	ret = 0;
 out_free:
	free(synthetic_event);
	free(hist);
	tracefs_put_tracing_file(path);
	return ret;
}
