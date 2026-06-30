#![allow (clippy::excessive_precision)]

pub fn Submit () -> u32
{
   crate::Submit_System (2, 3, "Halley's Comet System", 135, 2692763995184.9253, 679777129499.9795, 154241579121_i64, 16634585940_i64, 0.8844439707573535, 0.1552114115367666, 0.4398828808917623, 0.013089362784099112, 0.0, 0.0, 0.0, 0.0, 0.0, 0x00888888);

   crate::Submit_Body (3, 4, "Halley's Comet", 14, 5500.0, 0.0, 0x0088CCFF, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);

   crate::Submit_Surface (4, 5, "Halley's Comet Surface", "generic_moon-0.png", 0.0, 0_i64);

   crate::Submit_System (2, 6, "Comet Encke System", 135, 332638859718.0377, 176742689581.11786, 6696740960_i64, 3939030190_i64, 0.028075463513984537, -0.9812571618604092, -0.0977759145737259, -0.16366435523778897, 0.0, 0.0, 0.0, 0.0, 0.0, 0x00888888);

   crate::Submit_Body (6, 7, "Comet Encke", 14, 2400.0, 0.0, 0x0088CCFF, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);

   crate::Submit_Surface (7, 8, "Comet Encke Surface", "generic_moon-0.png", 0.0, 0_i64);

   crate::Submit_System (2, 9, "Comet Hale-Bopp System", 135, 28129631936448.54, 2770413419258.2446, 5207749666129_i64, 176210919_i64, 0.17090653854577575, -0.31739633852413773, -0.6825082438022869, -0.6357932183624566, 0.0, 0.0, 0.0, 0.0, 0.0, 0x00888888);

   crate::Submit_Body (9, 10, "Comet Hale-Bopp", 14, 30000.0, 0.0, 0x0088CCFF, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);

   crate::Submit_Surface (10, 11, "Comet Hale-Bopp Surface", "generic_moon-0.png", 0.0, 0_i64);

   9
}
