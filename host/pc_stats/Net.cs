// JSON and HTTP shared by both halves of ESP32-Helper.exe.
//
// Json.Parse returns the shapes JavaScriptSerializer.DeserializeObject gave on
// .NET Framework (Dictionary<string, object>, object[], int / long / decimal /
// double, string, bool, null), so the code that walks the result did not have
// to change. Json.Write takes the same shapes back.
//
// Http wraps one HttpClient. A non-2xx answer throws HttpFailure carrying the
// status code; a timeout or network error throws HttpFailure with Status 0 and
// the reason in Reason (what WebException.Status used to say).
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Threading;

static class Json {
  static readonly JsonDocumentOptions ReadOptions = new JsonDocumentOptions {
    AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip
  };
  // files we write back (Claude's credentials) keep non-ASCII text readable
  static readonly JsonSerializerOptions WriteOptions = new JsonSerializerOptions {
    Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping
  };

  public static object Parse(string json) {
    using (JsonDocument doc = JsonDocument.Parse(json, ReadOptions)) return FromElement(doc.RootElement);
  }

  public static Dictionary<string, object> ParseObject(string json) {
    return Parse(json) as Dictionary<string, object>;
  }

  public static string Write(object value) {
    return value == null ? "null" : JsonSerializer.Serialize(value, value.GetType(), WriteOptions);
  }

  static object FromElement(JsonElement e) {
    switch (e.ValueKind) {
      case JsonValueKind.Object:
        var d = new Dictionary<string, object>();
        foreach (JsonProperty p in e.EnumerateObject()) d[p.Name] = FromElement(p.Value);
        return d;
      case JsonValueKind.Array:
        var list = new List<object>();
        foreach (JsonElement item in e.EnumerateArray()) list.Add(FromElement(item));
        return list.ToArray();
      case JsonValueKind.String:
        return e.GetString();
      case JsonValueKind.Number:
        int i32;
        long i64;
        decimal dec;
        if (e.TryGetInt32(out i32)) return i32;
        if (e.TryGetInt64(out i64)) return i64;
        if (e.TryGetDecimal(out dec)) return dec;
        return e.GetDouble();
      case JsonValueKind.True:
        return true;
      case JsonValueKind.False:
        return false;
      default:
        return null;
    }
  }
}

class HttpFailure : Exception {
  public readonly int Status;      // HTTP status code, 0 when no answer came back
  public readonly string Reason;   // "Timeout", "Canceled", an HttpRequestError name, or the reason phrase

  public HttpFailure(int status, string reason) : base(status != 0 ? "HTTP " + status : reason) {
    Status = status;
    Reason = reason;
  }
}

static class Http {
  static HttpClient NewClient(bool useProxy) {
    return new HttpClient(new SocketsHttpHandler {
      UseProxy = useProxy,
      PooledConnectionIdleTimeout = TimeSpan.FromSeconds(30),
      AutomaticDecompression = DecompressionMethods.All
    }) { Timeout = Timeout.InfiniteTimeSpan };   // per request, below
  }

  static readonly HttpClient Client = NewClient(true);
  static readonly HttpClient Direct = NewClient(false);   // 127.0.0.1 never goes through a proxy

  // True when a TCP connection to host:443 opens within timeoutMs: the PC is
  // online and the host resolves. Lets callers wait out an outage instead of
  // spending a request that must not fail halfway (a rotating token).
  public static bool Reachable(string host, int timeoutMs) {
    try {
      using (var tcp = new System.Net.Sockets.TcpClient()) {
        return tcp.ConnectAsync(host, 443).Wait(timeoutMs) && tcp.Connected;
      }
    } catch (Exception) {
      return false;
    }
  }

  public static string Get(string url, IDictionary<string, string> headers, int timeoutMs, CancellationToken cancel = default(CancellationToken)) {
    return Send(HttpMethod.Get, url, null, headers, timeoutMs, cancel);
  }

  public static string Delete(string url, IDictionary<string, string> headers, int timeoutMs) {
    return Send(HttpMethod.Delete, url, null, headers, timeoutMs, default(CancellationToken));
  }

  // contentType goes out exactly as given (no "; charset=" added)
  public static string Post(string url, byte[] body, string contentType, IDictionary<string, string> headers, int timeoutMs) {
    var content = new ByteArrayContent(body);
    content.Headers.ContentType = MediaTypeHeaderValue.Parse(contentType);
    return Send(HttpMethod.Post, url, content, headers, timeoutMs, default(CancellationToken));
  }

  public static string PostJson(string url, string json, IDictionary<string, string> headers, int timeoutMs) {
    return Post(url, Encoding.UTF8.GetBytes(json), "application/json", headers, timeoutMs);
  }

  public static string PostForm(string url, string form, IDictionary<string, string> headers, int timeoutMs) {
    return Post(url, Encoding.ASCII.GetBytes(form), "application/x-www-form-urlencoded", headers, timeoutMs);
  }

  public static string Send(HttpMethod method, string url, HttpContent content, IDictionary<string, string> headers,
      int timeoutMs, CancellationToken cancel) {
    var uri = new Uri(url);
    using (var req = new HttpRequestMessage(method, uri) { Content = content })
    using (var cts = CancellationTokenSource.CreateLinkedTokenSource(cancel)) {
      if (headers != null) {
        foreach (KeyValuePair<string, string> h in headers) req.Headers.TryAddWithoutValidation(h.Key, h.Value);
      }
      cts.CancelAfter(timeoutMs);
      HttpResponseMessage resp;
      try {
        resp = (uri.IsLoopback ? Direct : Client).Send(req, HttpCompletionOption.ResponseContentRead, cts.Token);
      } catch (OperationCanceledException) {
        throw new HttpFailure(0, cancel.IsCancellationRequested ? "Canceled" : "Timeout");
      } catch (HttpRequestException ex) {
        throw new HttpFailure(0, ex.HttpRequestError.ToString());
      }
      using (resp)
      using (Stream s = resp.Content.ReadAsStream(cts.Token))
      using (var r = new StreamReader(s, Encoding.UTF8)) {
        string body = r.ReadToEnd();
        if (!resp.IsSuccessStatusCode) throw new HttpFailure((int)resp.StatusCode, resp.ReasonPhrase);
        return body;
      }
    }
  }
}
