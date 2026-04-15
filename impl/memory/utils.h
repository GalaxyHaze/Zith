//
// Created by diogo on 14/03/26.
//

#ifndef ZITH_UTILS_H
#define ZITH_UTILS_H

// impl/memory/utils.h — Arena utilities for Zith
#pragma once

#include <cstddef>
#include <cstring>

#include <zith/zith.hpp>

// ============================================================================
// ArenaList<T>
//
// Lista ligada de chunks alocados numa ZithArena.
// Não aloca heap — todos os blocos vivem na arena do chamador.
//
// Uso típico:
//
//   ArenaList<ZithToken> tokens;
//   tokens.init(arena, /*chunk_capacity=*/64);
//   tokens.push(arena, some_token);
//   ...
//   size_t count = 0;
//   ZithToken* flat = tokens.flatten(arena, &count);
//
// Invariantes:
//   - init() deve ser chamado antes de qualquer push()
//   - flatten() pode ser chamado zero ou mais vezes; não consome a lista
//   - a lista e o array produzido por flatten() partilham a mesma arena;
//     a lifetime de ambos é determinada pela arena
// ============================================================================

template<typename T>
struct ArenaList {
    // ── Chunk interno ────────────────────────────────────────────────────────
    //
    // O tamanho do array de items é variável: o chunk é alocado na arena com
    // espaço para 'capacity' elementos a seguir ao header.
    // Layout em memória:  [ Chunk header | T items[capacity] ]

    struct Chunk {
        Chunk *next;
        size_t len;
        size_t capacity;

        T *items() {
            // Os items estão imediatamente após o header
            return reinterpret_cast<T *>(this + 1);
        }

        const T *items() const {
            return reinterpret_cast<const T *>(this + 1);
        }
    };

    Chunk *head_ = nullptr;
    Chunk *tail_ = nullptr;
    size_t total_ = 0;
    size_t chunk_capacity_ = 0;

    // ── API pública ──────────────────────────────────────────────────────────

    // Inicializa a lista com a capacidade preferida por chunk.
    // Pode ser chamado mais de uma vez para reutilizar a struct (reset lógico).
    void init(ZithArena * /*arena*/, size_t chunk_capacity) {
        head_ = nullptr;
        tail_ = nullptr;
        total_ = 0;
        chunk_capacity_ = chunk_capacity > 0 ? chunk_capacity : 16;
    }

    // Número total de elementos inseridos
    size_t size() const { return total_; }

    bool empty() const { return total_ == 0; }

    // Insere um elemento no final da lista.
    // Aloca um novo chunk na arena se o atual estiver cheio.
    void push(ZithArena *arena, const T &value) {
        if (!tail_ || tail_->len == tail_->capacity) {
            alloc_chunk(arena);
        }
        tail_->items()[tail_->len++] = value;
        total_++;
    }

    // Produz um array contíguo alocado na arena com todos os elementos em ordem.
    // Retorna nullptr se a lista estiver vazia.
    // Pode ser chamado múltiplas vezes — cada chamada aloca um novo array.
    T *flatten(ZithArena *arena, size_t *out_count) const {
        *out_count = total_;
        if (total_ == 0) return nullptr;

        T *arr = static_cast<T *>(zith_arena_alloc(arena, total_ * sizeof(T)));
        if (!arr) {
            *out_count = 0;
            return nullptr;
        }

        size_t i = 0;
        for (const Chunk *c = head_; c; c = c->next) {
            std::memcpy(arr + i, c->items(), c->len * sizeof(T));
            i += c->len;
        }
        return arr;
    }

    // Acesso por índice — O(n/chunk_capacity), útil para debug
    // Não usar em hot paths; prefira flatten() para iteração
    T *at(size_t index) {
        for (Chunk *c = head_; c; c = c->next) {
            if (index < c->len) return &c->items()[index];
            index -= c->len;
        }
        return nullptr;
    }

    const T *at(size_t index) const {
        for (const Chunk *c = head_; c; c = c->next) {
            if (index < c->len) return &c->items()[index];
            index -= c->len;
        }
        return nullptr;
    }

    // Iterador para range-for sobre os chunks (sem flatten)
    struct Iterator {
        const Chunk *chunk;
        size_t index;

        bool operator!=(const Iterator &o) const {
            return chunk != o.chunk || index != o.index;
        }

        Iterator &operator++() {
            if (!chunk) return *this;
            if (++index >= chunk->len) {
                chunk = chunk->next;
                index = 0;
            }
            // Avança sobre chunks vazios (não deve ocorrer, mas defensivo)
            while (chunk && chunk->len == 0) { chunk = chunk->next; }
            return *this;
        }

        const T &operator*() const { return chunk->items()[index]; }
    };

    Iterator begin() const {
        // Avança até ao primeiro chunk com elementos
        const Chunk *c = head_;
        while (c && c->len == 0) c = c->next;
        return {c, 0};
    }

    Iterator end() const { return {nullptr, 0}; }

private:
    // Aloca um novo Chunk na arena e liga-o ao tail
    void alloc_chunk(ZithArena *arena) {
        const size_t alloc_size = sizeof(Chunk) + chunk_capacity_ * sizeof(T);
        auto *c = static_cast<Chunk *>(zith_arena_alloc(arena, alloc_size));
        if (!c) return; // arena esgotada — push seguinte será no-op

        c->next = nullptr;
        c->len = 0;
        c->capacity = chunk_capacity_;

        if (tail_) tail_->next = c;
        else head_ = c;
        tail_ = c;
    }
};

#endif //ZITH_UTILS_H