/*
 * caricoder3 - the technician: a local language model that says what it
 * thinks caused each incident
 * Copyright (c) 2026 CariTech Solutions
 *
 * llama.cpp's server (llama-server) with a model on this machine, started
 * when there is something to look at and stopped when it has been idle a
 * while; on an NVIDIA card with room for it, or on the CPU at the lowest
 * priority (it never competes with the channels). Nothing leaves the
 * machine. For each incident (a source's, not its channels' one by one)
 * it is given the knowledge folder (what it is, the network, what the
 * faults look like), the incident with its evidence and events, and past
 * incidents like it with what a person said they were; it answers with a
 * JSON object (cause, category, confidence, explanation, next checks, a
 * ticket for the provider) kept on the incident.
 */
#ifndef CC3_TECH_H
#define CC3_TECH_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct tech_conf {
    bool enabled;
    char server[512];           /* the llama-server binary */
    char model[512];            /* the .gguf */
    char knowledge[2][512];     /* folders of .md notes: the shipped ones, the site's own */
    char gpu[16];               /* "auto" (a card with room, else the CPU), "cpu", or a card's index */
    int threads;                /* CPU threads it may use */
    int ctx;                    /* context, tokens */
    int port;                   /* on 127.0.0.1 */
    int idle_s;                 /* stopped after this long with nothing to do */
    int delay_s;                /* an incident is looked at this long after it opens (evidence first) */
    char my_name[64];           /* this transcoder, for the report */
};

void tech_set(const struct tech_conf *c);
void tech_stop(void);

/* Look at incident `id` (after the delay; `now`: at once). */
void tech_queue(int64_t id, bool now);

/* The request the model would be sent about incident `id` (system and
 * user messages, schema): to read and improve the prompt. */
bool tech_prompt(FILE *f, int64_t id);

/* Training data: one JSON line per incident a person gave a verdict on
 * ({"incident", "verdict", "messages": [system, user, assistant]}, the
 * assistant's answer being what it should have said). Returns how many. */
int tech_export(FILE *f);

/* {"enabled", "state", "where", "model", "queue", "done", "last_error", ...} */
void tech_state_json(FILE *f);

#endif
