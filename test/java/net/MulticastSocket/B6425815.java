/*
 * Copyright 2006 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 */

/*
 * @test
 *
 * @bug 6425815
 *
 * @summary java.net.MulticastSocket.setTimeToLive(255) reports 'Socket closed' (WinXP, IPv6)
 *
 */
import java.net.*;
import java.io.IOException;

public class B6425815 {
    public static void main(String[] args) throws Exception {
        InetAddress ia;
        MulticastSocket ms;

        try {
            ia = InetAddress.getByName("::1");
            ms = new MulticastSocket(new InetSocketAddress(ia, 1234));
        } catch (Exception e) {
            // If this fails, it means the system doesn't have IPV6
            // support, therefore this test shouldn't be run.
            ia = null;
            ms = null;
        }
        if (ms != null) {
            ms.setTimeToLive(254);
            if (ms.getTimeToLive() != 254) {
                throw new RuntimeException("time to live is incorrect!");
            }
            ms.close();
        }
    }
}
