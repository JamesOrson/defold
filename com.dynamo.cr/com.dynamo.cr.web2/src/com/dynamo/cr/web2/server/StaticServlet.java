package com.dynamo.cr.web2.server;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.Serializable;
import java.net.URL;
import java.util.Arrays;
import java.util.logging.Level;

import javax.servlet.ServletContext;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

import com.google.appengine.api.memcache.ErrorHandlers;
import com.google.appengine.api.memcache.MemcacheService;
import com.google.appengine.api.memcache.MemcacheServiceFactory;

/**
 * Servlet for serving static content without the trailing .html
 * Use sindex.html for index-pages. This is in order to avoid conflicts with default
 * index-page index.html served by googles CDN.
 * Memcache is used to cache pages. (necessary?)
 * @author chmu
 *
 */
public class StaticServlet extends HttpServlet {
    private static final long serialVersionUID = -7462855764457995784L;
    private MemcacheService syncCache;

    public StaticServlet() {
        super();
        syncCache = MemcacheServiceFactory.getMemcacheService();
        /*
         * NOTE: We cache static resource by the key and the memcache
         * database is alive over deployments. It's crucial that we flush
         * the cache when a new version is deployed. This flushing scheme
         * is suboptimal as we will also flush the cache every time a new instance
         * is spawned. But it's better to be safe than sorry :-)
         * A better solution would be to prefix the cache key with something
         * unique to the current deployment but I couldn't find any value in appengine
         * appropriate.
         */
        syncCache.clearAll();
        syncCache.setErrorHandler(ErrorHandlers.getConsistentLogAndContinue(Level.INFO));
    }

    private static class Page implements Serializable {
        private static final long serialVersionUID = 1L;

        byte[] content;
        String etag;

        Page(byte[] content) {
            this.content = content;
            int hashCode = Arrays.hashCode(content);
            etag = Integer.toString(hashCode);

        }
    }

    public void doGet(HttpServletRequest req, HttpServletResponse resp)
            throws IOException {

        URL url = new URL(req.getRequestURL().toString());

        ServletContext context = getServletContext();
        String path = String.format("%s.html", url.getPath());
        String index_path = String.format("%s/sindex.html", url.getPath());

        Page page = (Page) syncCache.get(path);
        if (page == null) {
            InputStream stream = context.getResourceAsStream(path);

            if (stream == null) {
                // Try to load .../index.html instead
                stream = context.getResourceAsStream(index_path);
            }

            if (stream == null) {
                resp.sendError(HttpServletResponse.SC_NOT_FOUND);
                return;
            }

            page = new Page(loadResource(stream));
            if (!url.getHost().equals("127.0.0.1")) {
                // Cache only on production server, i.e. != 127.0.0.1
                syncCache.put(path, page);
            }
        }
        resp.setContentType("text/html");
        if (!url.getHost().equals("127.0.0.1")) {
            // Only cache control for production server, i.e. != 127.0.0.1
            resp.setHeader("Cache-Control", "public, max-age=600");
            resp.setHeader("ETag", page.etag);
        }
        resp.getOutputStream().write(page.content);
    }

    private byte[] loadResource(InputStream stream) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream(1024 * 128);
        byte[] buf = new byte[1024 * 128];
        int n = stream.read(buf);
        while (n > 0) {
            output.write(buf, 0, n);
            n = stream.read(buf);
        }
        stream.close();
        output.close();
        return output.toByteArray();
    }
}