#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include <sys/types.h>

// Crée le sémaphore, valeur de départ = 1 (libre)
int semaphore_create(key_t key);

// Prend le verrou : -1, si c'est occupé, on attend
int semaphore_lock(int semid);

// Libère le verrou : +1
int semaphore_unlock(int semid);

// Supprime le sémaphore à la fin, pour ne pas laisser de déchets
int semaphore_remove(int semid);

#endif
