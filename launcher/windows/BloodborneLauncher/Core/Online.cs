using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text;

namespace BloodborneLauncher.Core
{
    /// <summary>The online connection test: the game server (messages, bloodstains, ghosts), the
    /// shadNet server's TCP port (sign-in, co-op), its Web API, and UDP to it (summons).</summary>
    public static class Online
    {
        public sealed record Result(string Name, string Address, bool Ok, string Detail);

        /// <summary>The game server as scripts/online.py uses it: empty is The Hunter's Dream, a
        /// bare name gets https://, and the old default (the public shadNet Web API, which has no
        /// game server) becomes The Hunter's Dream.</summary>
        public static string GameServer(string text)
        {
            text = text.Trim().TrimEnd('/');
            if (text.Length == 0) return LauncherSettings.OnlineCommunity;
            if (!text.Contains("://")) text = "https://" + text;
            return Uri.TryCreate(text, UriKind.Absolute, out var uri) && uri.Host == "srv.shadps4.net"
                ? LauncherSettings.OnlineCommunity
                : text;
        }

        public static async Task<List<Result>> CheckAsync(string gameServer, string server, string webapi)
        {
            var results = new List<Result> { await CheckGameServerAsync(gameServer) };
            try
            {
                var at = server.LastIndexOf(':');
                var host = at > 0 ? server[..at] : server;
                var port = at > 0 && int.TryParse(server[(at + 1)..], out var p) ? p : 31313;
                using var tcp = new TcpClient();
                using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                await tcp.ConnectAsync(host, port, cancel.Token);
                results.Add(new Result("Co-op server", server, true, ""));
            }
            catch (Exception e)
            {
                results.Add(new Result("Co-op server", server, false, e.GetBaseException().Message));
            }

            try
            {
                using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(5) };
                var body = await http.GetStringAsync(webapi.TrimEnd('/') + "/status");
                var ok = body.Replace(" ", "").Contains("\"ok\":true");
                results.Add(new Result("Web API", webapi, ok, ok ? "" : "not a shadNet server"));
            }
            catch (Exception e)
            {
                results.Add(new Result("Web API", webapi, false, e.GetBaseException().Message));
            }
            return results;
        }

        /// <summary>
        /// The path summons take: the game's signaling ping to the shadNet server's UDP port (its
        /// TCP port + 1), command 1 with the account name, framed both ways the engine sends it
        /// (the shadPS4 server's PS4 datagram [FF 83 FF FF FF FF], a shadp2p server's 4-byte vport
        /// header). The server answers with the address it saw: UDP gets through, and that is the
        /// address other players are given. Sent from the game's P2P port when it is free, so the
        /// router's mapping for that port is the one tested. The public server answers only players
        /// signed in from the game, so no answer proves nothing: null then.
        /// </summary>
        public static async Task<Result?> CheckUdpAsync(string host, int tcpPort, string account, int localPort)
        {
            var address = $"{host}:{tcpPort + 1} (UDP)";
            try
            {
                var ip = (await Dns.GetHostAddressesAsync(host)).First(a => a.AddressFamily == AddressFamily.InterNetwork);
                UdpClient udp;
                try { udp = new UdpClient(localPort); }
                catch (SocketException) { udp = new UdpClient(0); } // the game holds it
                using (udp)
                {
                    var ping = new byte[21];
                    ping[0] = 0x01;
                    var name = Encoding.UTF8.GetBytes(account);
                    Array.Copy(name, 0, ping, 1, Math.Min(name.Length, 16));
                    byte[] Framed(params byte[] header) => header.Concat(ping).ToArray();
                    var ps4 = Framed(0xFF, 0x83, 0xFF, 0xFF, 0xFF, 0xFF);
                    var shadp2p = Framed(0xFF, 0xFF, 0xFF, 0xFF);
                    var server = new IPEndPoint(ip, tcpPort + 1);
                    for (var attempt = 0; attempt < 3; ++attempt)
                    {
                        await udp.SendAsync(ps4, ps4.Length, server);
                        await udp.SendAsync(shadp2p, shadp2p.Length, server);
                        using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(2));
                        try
                        {
                            var b = (await udp.ReceiveAsync(cancel.Token)).Buffer;
                            var at = b.Length >= 12 && b[0] == 0xFF && b[1] == 0x83 ? 6
                                : b.Length >= 10 && b[0] == 0xFF && b[1] == 0xFF ? 4 : -1;
                            if (at < 0) continue;
                            var external = new IPAddress(new ReadOnlySpan<byte>(b, at, 4));
                            var port = (b[at + 4] << 8) | b[at + 5];
                            return new Result("Co-op UDP", address, true, $"the server sees you at {external}:{port}");
                        }
                        catch (OperationCanceledException)
                        {
                        }
                    }
                    return null;
                }
            }
            catch (Exception e)
            {
                return new Result("Co-op UDP", address, false, e.GetBaseException().Message);
            }
        }

        /// <summary>The game's first request: its server information (ss.info, base64 XML that
        /// lists the game APIs). Without a port, the game's own (20443) is used.</summary>
        private static async Task<Result> CheckGameServerAsync(string gameServer)
        {
            try
            {
                var uri = new UriBuilder(gameServer);
                if (new Uri(gameServer).IsDefaultPort) uri.Port = 20443;
                uri.Path = "/bb-eu/ss.info";
                using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(7) };
                var body = await http.GetStringAsync(uri.Uri);
                var xml = Encoding.UTF8.GetString(Convert.FromBase64String(body.Trim()));
                var ok = xml.Contains("<api_", StringComparison.Ordinal);
                return new Result("Game server", gameServer, ok, ok ? "" : "not a Bloodborne game server");
            }
            catch (FormatException)
            {
                return new Result("Game server", gameServer, false, "not a Bloodborne game server");
            }
            catch (Exception e)
            {
                return new Result("Game server", gameServer, false, e.GetBaseException().Message);
            }
        }
    }
}
