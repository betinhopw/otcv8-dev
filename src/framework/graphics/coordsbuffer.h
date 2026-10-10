/*
 * Copyright (c) 2010-2017 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef COORDSBUFFER_H
#define COORDSBUFFER_H

#include "vertexarray.h"

class CoordsBuffer
{
public:
    CoordsBuffer();
    ~CoordsBuffer();
    CoordsBuffer(CoordsBuffer& c) = delete;
    CoordsBuffer& operator=(CoordsBuffer& c) = delete;
    CoordsBuffer(CoordsBuffer&& c) noexcept : m_vertexArray(c.m_vertexArray), m_textureCoordArray(c.m_textureCoordArray)
    {
        m_locked = c.m_locked = true;
    };

    void clear() {
        if (m_locked)
            unlock(true);
        m_textureCoordArray->clear();
        m_vertexArray->clear();
    }

    void addTriangle(const Point& a, const Point& b, const Point& c) {
        if (m_locked)
            unlock();
        m_vertexArray->addTriangle(a, b, c);
    }
    // Ethedor: triangulo texturizado com coordenadas em float (usado pelo minimapa redondo)
    void addTexturedTriangle(const PointF& a, const PointF& b, const PointF& c, const PointF& ta, const PointF& tb, const PointF& tc) {
        if (m_locked)
            return;
        m_vertexArray->addVertex(a.x, a.y);
        m_vertexArray->addVertex(b.x, b.y);
        m_vertexArray->addVertex(c.x, c.y);
        m_textureCoordArray->addVertex(ta.x, ta.y);
        m_textureCoordArray->addVertex(tb.x, tb.y);
        m_textureCoordArray->addVertex(tc.x, tc.y);
    }
    void addFillTriangle(const PointF& a, const PointF& b, const PointF& c) {
        if (m_locked)
            return;
        m_vertexArray->addVertex(a.x, a.y);
        m_vertexArray->addVertex(b.x, b.y);
        m_vertexArray->addVertex(c.x, c.y);
    }
    void addRect(const Rect& dest) {
        if (m_locked)
            unlock();
        m_vertexArray->addRect(dest);
    }
    void addRect(const Rect& dest, const Rect& src)
    {
        if (m_locked)
            unlock();
        m_vertexArray->addRect(dest);
        m_textureCoordArray->addRect(src);
    }
    void addRect(const RectF& dest, const RectF& src)
    {
        if (m_locked)
            unlock();
        m_vertexArray->addRect(dest);
        m_textureCoordArray->addRect(src);
    }
    void addQuad(const Rect& dest, const Rect& src) {
        if (m_locked)
            unlock();
        m_vertexArray->addQuad(dest);
        m_textureCoordArray->addQuad(src);
    }
    void addUpsideDownQuad(const Rect& dest, const Rect& src) {
        if (m_locked)
            unlock();
        m_vertexArray->addUpsideDownQuad(dest);
        m_textureCoordArray->addQuad(src);
    }

    void addBoudingRect(const Rect& dest, int innerLineWidth);
    void addRepeatedRects(const Rect& dest, const Rect& src);

    float *getVertexArray() { return m_vertexArray->vertices(); }
    float *getTextureCoordArray() { return m_textureCoordArray->vertices(); }
    int getVertexCount() { return m_vertexArray->vertexCount(); }
    int getTextureCoordCount() { return m_textureCoordArray->vertexCount(); }
    HardwareBuffer* getVertexHardwareCache() { return m_vertexArray->getHardwareCache(); }
    HardwareBuffer* getTextureHardwareCache() { return m_textureCoordArray->getHardwareCache(); }

    void unlock(bool clear = false);
    void cache()
    {
        m_locked = true;
        m_vertexArray->cache();
        m_textureCoordArray->cache();
    }
    Rect getTextureRect();

private:
    bool m_locked = false;
    std::shared_ptr<VertexArray> m_vertexArray;
    std::shared_ptr<VertexArray> m_textureCoordArray;
};

#endif
